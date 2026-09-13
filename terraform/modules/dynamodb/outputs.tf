output "table_name" {
  value = aws_dynamodb_table.device_shadows.name
}

output "table_arn" {
  value = aws_dynamodb_table.device_shadows.arn
}
