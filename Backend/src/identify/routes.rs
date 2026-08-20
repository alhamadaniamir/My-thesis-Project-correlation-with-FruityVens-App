use std::time::Duration;

use actix_web::{get, post, web, HttpResponse};
use serde::{Deserialize, Serialize};

use crate::config::Config;
use crate::identify::fruit::canonical_fruit;
use crate::identify::gemini::transcribe_fruit;

/// Gemini normally answers in 3 to 7 seconds. The scale gives up at 15
/// (kBackendHttpTimeoutMs), so cap the upstream call below that: a silent
/// turn never sends turnComplete and would otherwise hang the container
/// until the gateway kills it.
const GEMINI_TIMEOUT: Duration = Duration::from_secs(12);

#[derive(Deserialize)]
pub struct IdentifyRequest {
    pub mime_type: String,
    pub data: String,
}

#[derive(Serialize)]
pub struct IdentifyResponse {
    pub fruit: String,
}

#[post("/api/identify")]
pub async fn identify(
    request: web::Json<IdentifyRequest>,
    config: web::Data<Config>,
) -> HttpResponse {
    if config.gemini_api_key.is_empty() {
        eprintln!("GEMINI_API_KEY is not set");
        return HttpResponse::InternalServerError().body("not configured");
    }

    let call = transcribe_fruit(&config.gemini_api_key, &request.mime_type, &request.data);

    match tokio::time::timeout(GEMINI_TIMEOUT, call).await {
        Ok(Ok(transcript)) => HttpResponse::Ok().json(IdentifyResponse {
            fruit: canonical_fruit(&transcript).to_string(),
        }),
        Ok(Err(error)) => {
            eprintln!("gemini {} failed: {}", error.stage, error.detail);
            HttpResponse::BadGateway().body(format!("upstream {} failed", error.stage))
        }
        Err(_) => {
            eprintln!("gemini timed out after {}s", GEMINI_TIMEOUT.as_secs());
            HttpResponse::GatewayTimeout().body("upstream timed out")
        }
    }
}

pub const HEALTH_PATH: &str = "/health";

/// Unauthenticated so the platform can probe the container without a token.
#[get("/health")]
pub async fn health() -> HttpResponse {
    HttpResponse::Ok().body("ok")
}
