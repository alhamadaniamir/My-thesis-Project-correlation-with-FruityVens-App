use actix_web::{
    body::MessageBody,
    dev::{ServiceRequest, ServiceResponse},
    error::ErrorUnauthorized,
    http::header,
    middleware::Next,
    web, Error,
};

use crate::config::Config;
use crate::identify::routes::HEALTH_PATH;

/// Rejects anything without a matching bearer token. An unset or blank
/// AUTH_TOKEN rejects every request rather than letting an empty header
/// compare equal to an empty expected value.
pub async fn require_bearer(
    request: ServiceRequest,
    next: Next<impl MessageBody>,
) -> Result<ServiceResponse<impl MessageBody>, Error> {
    if request.path() == HEALTH_PATH {
        return next.call(request).await;
    }

    let expected = request
        .app_data::<web::Data<Config>>()
        .map(|config| config.auth_token.as_str())
        .unwrap_or_default();

    let provided = request
        .headers()
        .get(header::AUTHORIZATION)
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.strip_prefix("Bearer "))
        .unwrap_or_default();

    if expected.is_empty() || provided != expected {
        return Err(ErrorUnauthorized("unauthorized"));
    }

    next.call(request).await
}
