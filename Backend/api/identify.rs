use axum::{
    extract::DefaultBodyLimit,
    http::{header, HeaderValue, Method, Request, StatusCode},
    middleware::{self, Next},
    response::Response,
    routing::post,
    Json, Router,
};
use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::env;
use std::sync::OnceLock;
use tokio_tungstenite::{connect_async, tungstenite::Message};
use tower::ServiceBuilder;
use tower_http::cors::CorsLayer;
use vercel_runtime::{axum::VercelLayer, Error};

#[derive(Deserialize)]
struct IdentifyRequest {
    mime_type: String,
    data: String,
}

#[derive(Serialize)]
struct IdentifyResponse {
    fruit: String,
}

static AUTH_TOKEN: OnceLock<String> = OnceLock::new();

async fn auth(req: Request<axum::body::Body>, next: Next) -> Result<Response, (StatusCode, String)> {
    let expected = AUTH_TOKEN
        .get()
        .ok_or((StatusCode::INTERNAL_SERVER_ERROR, "AUTH_TOKEN not set".to_string()))?;

    let provided = req
        .headers()
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
        .unwrap_or("");

    if provided != expected.as_str() {
        return Err((StatusCode::UNAUTHORIZED, "unauthorized".to_string()));
    }
    Ok(next.run(req).await)
}

async fn identify(
    Json(req): Json<IdentifyRequest>,
) -> Result<Json<IdentifyResponse>, (StatusCode, String)> {
    let api_key = env::var("GEMINI_API_KEY")
        .map_err(|_| (StatusCode::INTERNAL_SERVER_ERROR, "GEMINI_API_KEY not set".to_string()))?;

    let model = "gemini-3.1-flash-live-preview";
    let url = format!(
        "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={}",
        api_key
    );

    let (mut ws, _) = connect_async(&url)
        .await
        .map_err(|e| (StatusCode::BAD_GATEWAY, format!("ws connect failed: {e}")))?;

    let setup = json!({
        "setup": {
            "model": format!("models/{}", model),
            "generationConfig": { "responseModalities": ["AUDIO"] },
            "outputAudioTranscription": {}
        }
    });
    ws.send(Message::Text(setup.to_string()))
        .await
        .map_err(|e| (StatusCode::BAD_GATEWAY, format!("ws send setup: {e}")))?;

    // Wait for setupComplete
    loop {
        let msg = ws
            .next()
            .await
            .ok_or((StatusCode::BAD_GATEWAY, "ws closed before setupComplete".to_string()))?
            .map_err(|e| (StatusCode::BAD_GATEWAY, format!("ws recv: {e}")))?;
        if let Some(text) = msg_text(&msg) {
            let v: Value = serde_json::from_str(&text).unwrap_or(Value::Null);
            if v.get("setupComplete").is_some() {
                break;
            }
        }
    }

    let prompt = "Identify the fruit in this image. Reply with exactly one label from this list and nothing else: Apple, Mango Carabao, Indian Mango, Apple Mango, Dragon Fruit, Watermelon, Pineapple, Mangosteen, Banana, Avocado, Grapes, Durian, Pomelo, Pear, Lemon, Orange, Unknown. Never reply with the generic word Mango on its own. If the fruit is a mango, judge the silhouette and the skin color, not the size in frame, and pick one variety: Mango Carabao is long and slender with an S curve and a pointed beak at the tip, thin smooth skin, pale green when unripe and deep golden yellow when ripe; Indian Mango is flat and thin in profile with a hooked kidney like curve, waxy deep green skin, usually sold unripe; Apple Mango is short, round and plump like an apple, with a red, pink or orange blush over yellow skin. If two varieties look equally likely, or the fruit is not on the list, reply Unknown. No punctuation, no explanation, no full sentence.";
    let client_content = json!({
        "clientContent": {
            "turns": [{
                "role": "user",
                "parts": [
                    { "text": prompt },
                    { "inlineData": { "mimeType": req.mime_type, "data": req.data } }
                ]
            }],
            "turnComplete": true
        }
    });
    ws.send(Message::Text(client_content.to_string()))
        .await
        .map_err(|e| (StatusCode::BAD_GATEWAY, format!("ws send content: {e}")))?;

    let mut out = String::new();
    loop {
        let msg = ws
            .next()
            .await
            .ok_or((StatusCode::BAD_GATEWAY, "ws closed before turnComplete".to_string()))?
            .map_err(|e| (StatusCode::BAD_GATEWAY, format!("ws recv: {e}")))?;
        let Some(text) = msg_text(&msg) else { continue };
        let v: Value = match serde_json::from_str(&text) {
            Ok(v) => v,
            Err(_) => continue,
        };

        if let Some(t) = v
            .pointer("/serverContent/outputTranscription/text")
            .and_then(|t| t.as_str())
        {
            out.push_str(t);
        }

        if v.pointer("/serverContent/turnComplete")
            .and_then(|b| b.as_bool())
            .unwrap_or(false)
        {
            break;
        }
    }

    let _ = ws.close(None).await;

    let fruit = canonical_fruit(&out).to_string();
    Ok(Json(IdentifyResponse { fruit }))
}

const FRUIT_ALIASES: &[(&str, &str)] = &[
    ("mangocarabao", "Mango Carabao"),
    ("carabaomango", "Mango Carabao"),
    ("carabao", "Mango Carabao"),
    ("philippinemango", "Mango Carabao"),
    ("manilamango", "Mango Carabao"),
    ("indianmango", "Indian Mango"),
    ("mangoindian", "Indian Mango"),
    ("applemango", "Apple Mango"),
    ("mangoapple", "Apple Mango"),
    ("mango", "Unknown"),
    ("apple", "Apple"),
    ("dragonfruit", "Dragon Fruit"),
    ("pitaya", "Dragon Fruit"),
    ("pitahaya", "Dragon Fruit"),
    ("watermelon", "Watermelon"),
    ("pineapple", "Pineapple"),
    ("mangosteen", "Mangosteen"),
    ("banana", "Banana"),
    ("avocado", "Avocado"),
    ("grapes", "Grapes"),
    ("grape", "Grapes"),
    ("durian", "Durian"),
    ("pomelo", "Pomelo"),
    ("pummelo", "Pomelo"),
    ("pear", "Pear"),
    ("lemon", "Lemon"),
    ("lime", "Lemon"),
    ("orange", "Orange"),
    ("mandarin", "Orange"),
    ("tangerine", "Orange"),
    ("clementine", "Orange"),
];

fn canonical_fruit(raw: &str) -> &'static str {
    let normalized: String = raw
        .chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .map(|c| c.to_ascii_lowercase())
        .collect();

    let mut best: Option<(usize, &'static str)> = None;
    for (alias, label) in FRUIT_ALIASES {
        if normalized.contains(alias) && best.is_none_or(|(len, _)| alias.len() > len) {
            best = Some((alias.len(), label));
        }
    }
    best.map(|(_, label)| label).unwrap_or("Unknown")
}

fn msg_text(msg: &Message) -> Option<String> {
    match msg {
        Message::Text(t) => Some(t.clone()),
        Message::Binary(b) => std::str::from_utf8(b).ok().map(|s| s.to_string()),
        _ => None,
    }
}

#[tokio::main]
async fn main() -> Result<(), Error> {
    let _ = dotenvy::dotenv();
    let _ = rustls::crypto::ring::default_provider().install_default();

    if let Ok(t) = env::var("AUTH_TOKEN") {
        let _ = AUTH_TOKEN.set(t);
    }

    let cors_origin = env::var("CORS_ORIGIN").unwrap_or_else(|_| "*".to_string());
    let cors = if cors_origin == "*" {
        CorsLayer::permissive()
    } else {
        let origins: Vec<HeaderValue> = cors_origin
            .split(',')
            .filter_map(|s| s.trim().parse().ok())
            .collect();
        CorsLayer::new()
            .allow_origin(origins)
            .allow_methods([Method::GET, Method::POST, Method::OPTIONS])
            .allow_headers([header::CONTENT_TYPE, header::AUTHORIZATION])
    };

    let router = Router::new()
        .route("/api/identify", post(identify))
        .layer(middleware::from_fn(auth))
        .layer(cors)
        .layer(DefaultBodyLimit::max(20 * 1024 * 1024));

    let app = ServiceBuilder::new()
        .layer(VercelLayer::new())
        .service(router);

    vercel_runtime::run(app).await
}
