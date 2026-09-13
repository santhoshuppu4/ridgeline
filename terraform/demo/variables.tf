variable "aws_region" {
  type    = string
  default = "us-west-2"
}

variable "name_prefix" {
  type    = string
  default = "ridgeline"
}

variable "lambda_image_uri" {
  type        = string
  description = "ECR image URI for the demo Lambda, built from Dockerfile.demo-lambda. No default -- must be built and pushed manually first; see demo/README.md."
}

variable "alert_email" {
  type        = string
  description = "Email address to receive the $1 hard budget alert. No default -- must be your own real address."
}
