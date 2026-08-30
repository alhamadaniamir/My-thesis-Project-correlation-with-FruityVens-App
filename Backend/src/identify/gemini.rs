use std::fmt::Display;

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio_tungstenite::{connect_async, tungstenite::Message};

const MODEL: &str = "gemini-3.1-flash-live-preview";

const PROMPT: &str = "Identify the fruit in this image. Reply with exactly one label from this list and nothing else: Apple, Mango Carabao, Indian Mango, Apple Mango, Dragon Fruit, Watermelon, Melon, Pineapple, Mangosteen, Banana, Avocado, Grapes, Durian, Pomelo, Pear, Lemon, Orange, Papaya, Calamansi, Rambutan, Lanzones, Guyabano, Strawberries, Unknown. Never reply with the generic word Mango on its own. Melon means cantaloupe or honeydew; a big green striped melon with red flesh is Watermelon. If the fruit is a mango, judge the silhouette and the skin color, not the size in frame, and pick one variety: Mango Carabao is long and slender with an S curve and a pointed beak at the tip, thin smooth skin, pale green when unripe and deep golden yellow when ripe; Indian Mango is flat and thin in profile with a hooked kidney like curve, waxy deep green skin, usually sold unripe; Apple Mango is a mango: round and plump, its skin covered in fine pale speckles, a red or orange blush fading smoothly into yellow, the stem sitting on a smooth unbroken shoulder with no well and no dimple at the base. If two varieties look equally likely, or the fruit is not on the list, reply Unknown. A fruit with its stem sunk in a well at the top and a dimple at the base is Apple. Pomelo and Pear are often confused: Pomelo is oversized, wider than two fists side by side, with a thick spongy rind often over half an inch, pale green to yellow, sometimes pear shaped but too big and thick skinned to hold like a pear; Pear is fist sized, thin smooth skin with light russet dots, no spongy rind, green, yellow, red, or brown. Durian and Guyabano both have a spiky husk: Durian has hard sharp rigid thorns on a heavy oval husk; Guyabano has soft short curved spines on a lighter dull green heart shaped fruit. Lanzones grows in small oval clusters with a dull brown or tan peel and gets mistaken for Grapes, which are smaller, smooth, and glossy. Papaya is large and elongated with smooth green to orange skin and gets mistaken for an unripe Mango Carabao; Papaya is bigger and blunt at both ends, Mango Carabao is smaller with a pointed beak tip and an S curved silhouette. No punctuation, no explanation, no full sentence.";

/// The stage that failed, plus a detail string with the API key already
/// redacted. The caller logs the detail and returns only the stage, so the key
/// can never reach the response body.
pub struct GeminiError {
    pub stage: &'static str,
    pub detail: String,
}

impl GeminiError {
    fn new(stage: &'static str, detail: impl Display, api_key: &str) -> Self {
        let mut detail = detail.to_string();
        if !api_key.is_empty() {
            detail = detail.replace(api_key, "<redacted>");
        }
        Self { stage, detail }
    }
}

fn message_text(message: &Message) -> Option<String> {
    match message {
        Message::Text(text) => Some(text.clone()),
        Message::Binary(bytes) => std::str::from_utf8(bytes).ok().map(|s| s.to_string()),
        _ => None,
    }
}

/// Asks Gemini what the image contains and returns the raw transcript.
/// The reply arrives as audio transcription, so it may carry punctuation or be
/// a whole sentence; normalizing it is the caller's job.
pub async fn transcribe_fruit(
    api_key: &str,
    mime_type: &str,
    data: &str,
) -> Result<String, GeminiError> {
    let url = format!(
        "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key={}",
        api_key
    );

    let (mut ws, _) = connect_async(&url)
        .await
        .map_err(|e| GeminiError::new("connect", e, api_key))?;

    let setup = json!({
        "setup": {
            "model": format!("models/{}", MODEL),
            "generationConfig": { "responseModalities": ["AUDIO"] },
            "outputAudioTranscription": {}
        }
    });
    ws.send(Message::Text(setup.to_string()))
        .await
        .map_err(|e| GeminiError::new("send setup", e, api_key))?;

    loop {
        let message = ws
            .next()
            .await
            .ok_or_else(|| GeminiError::new("setup", "closed before setupComplete", api_key))?
            .map_err(|e| GeminiError::new("receive", e, api_key))?;
        if let Some(text) = message_text(&message) {
            let value: Value = serde_json::from_str(&text).unwrap_or(Value::Null);
            if value.get("setupComplete").is_some() {
                break;
            }
        }
    }

    let turn = json!({
        "clientContent": {
            "turns": [{
                "role": "user",
                "parts": [
                    { "text": PROMPT },
                    { "inlineData": { "mimeType": mime_type, "data": data } }
                ]
            }],
            "turnComplete": true
        }
    });
    ws.send(Message::Text(turn.to_string()))
        .await
        .map_err(|e| GeminiError::new("send content", e, api_key))?;

    let mut transcript = String::new();
    loop {
        let message = ws
            .next()
            .await
            .ok_or_else(|| GeminiError::new("reply", "closed before turnComplete", api_key))?
            .map_err(|e| GeminiError::new("receive", e, api_key))?;
        let Some(text) = message_text(&message) else {
            continue;
        };
        let Ok(value) = serde_json::from_str::<Value>(&text) else {
            continue;
        };

        if let Some(chunk) = value
            .pointer("/serverContent/outputTranscription/text")
            .and_then(|t| t.as_str())
        {
            transcript.push_str(chunk);
        }

        if value
            .pointer("/serverContent/turnComplete")
            .and_then(|b| b.as_bool())
            .unwrap_or(false)
        {
            break;
        }
    }

    let _ = ws.close(None).await;
    Ok(transcript)
}
