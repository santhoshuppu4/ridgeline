output "api_endpoint" {
  value       = aws_apigatewayv2_api.demo.api_endpoint
  description = "Base URL. The frontend calls this base URL plus /events (GET) and /trigger (POST)."
}

output "ecr_repository_url" {
  value = aws_ecr_repository.demo_lambda.repository_url
}

output "dynamodb_table_name" {
  value = aws_dynamodb_table.demo_events.name
}
