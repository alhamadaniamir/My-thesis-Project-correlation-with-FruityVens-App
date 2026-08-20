mod auth;
mod config;
mod identify;

use std::env;

use actix_cors::Cors;
use actix_web::{http::header, middleware::from_fn, web, App, HttpServer};

use crate::config::Config;

const DEFAULT_PORT: u16 = 80;
const MAX_BODY_BYTES: usize = 20 * 1024 * 1024;

#[actix_web::main]
async fn main() -> std::io::Result<()> {
    let _ = dotenvy::dotenv();
    let _ = rustls::crypto::ring::default_provider().install_default();

    let gemini_api_key = env::var("GEMINI_API_KEY").unwrap_or_default();
    if gemini_api_key.is_empty() {
        eprintln!("GEMINI_API_KEY missing; /api/identify will answer 500");
    }

    let auth_token = env::var("AUTH_TOKEN").unwrap_or_default();
    if auth_token.trim().is_empty() {
        eprintln!("AUTH_TOKEN missing or empty; every request will be rejected");
    }

    let cors_origin = env::var("CORS_ORIGIN").unwrap_or_else(|_| "*".to_string());
    let port = env::var("PORT")
        .ok()
        .and_then(|port| port.parse().ok())
        .unwrap_or(DEFAULT_PORT);

    let config = web::Data::new(Config {
        gemini_api_key,
        auth_token,
    });

    eprintln!("listening on 0.0.0.0:{port}");

    HttpServer::new(move || {
        let cors = if cors_origin == "*" {
            Cors::permissive()
        } else {
            cors_origin.split(',').fold(
                Cors::default()
                    .allowed_methods(["GET", "POST", "OPTIONS"])
                    .allowed_headers([header::CONTENT_TYPE, header::AUTHORIZATION]),
                |cors, origin| cors.allowed_origin(origin.trim()),
            )
        };

        App::new()
            .app_data(config.clone())
            .app_data(web::JsonConfig::default().limit(MAX_BODY_BYTES))
            .service(identify::routes::identify)
            .service(identify::routes::health)
            .wrap(from_fn(auth::require_bearer))
            .wrap(cors)
    })
    .shutdown_timeout(25)
    .bind(("0.0.0.0", port))?
    .run()
    .await
}
