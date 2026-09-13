variable "name_prefix" {
  type = string
}
variable "aws_region" {
  type = string
}
variable "subnet_ids" {
  type = list(string)
}
variable "security_group_id" {
  type = string
}
variable "gateway_image" {
  type        = string
  description = "ECR image URI. Must be built and pushed manually first -- see terraform/README.md."
}
variable "gateway_port" {
  type    = number
  default = 50051
}
variable "task_cpu" {
  type    = number
  default = 512  # 0.5 vCPU -- a real Fargate-valid value, sized for a low-traffic portfolio deployment, not a placeholder.
}
variable "task_memory" {
  type    = number
  default = 1024  # 1 GB -- must be a valid CPU/memory pairing per Fargate's documented combinations.
}
variable "desired_count" {
  type    = number
  default = 1
}
variable "dynamodb_table_arn" {
  type = string
}
variable "msk_cluster_arn" {
  type = string
}
variable "ota_bucket_arn" {
  type = string
}
