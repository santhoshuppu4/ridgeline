variable "aws_region" {
  type    = string
  default = "us-west-2"  # Matches DynamoDbConfig::region's own default -- see terraform/modules/dynamodb/main.tf's header comment.
}

variable "name_prefix" {
  type        = string
  default     = "ridgeline"
  description = "Prefix applied to every resource name/tag, so multiple environments (dev/staging) don't collide."
}

variable "availability_zones" {
  type        = list(string)
  default     = ["us-west-2a", "us-west-2b"]
  description = "Must be real AZs within var.aws_region. Two, for the multi-AZ placement MSK/ElastiCache/ECS all want."
}

variable "gateway_port" {
  type    = number
  default = 50051
}

variable "dynamodb_table_name" {
  type    = string
  default = "device_shadows"
}

variable "redis_node_type" {
  type    = string
  default = "cache.t4g.micro"
}

variable "gateway_image" {
  type        = string
  description = "ECR image URI for the gateway container. No default -- must be built and pushed manually first; see terraform/README.md."
}
