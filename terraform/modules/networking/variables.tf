variable "name_prefix" {
  type        = string
  description = "Prefix applied to every resource name/tag in this module."
}

variable "vpc_cidr" {
  type    = string
  default = "10.42.0.0/16"
}

variable "availability_zones" {
  type        = list(string)
  description = "At least 2, for the multi-AZ placement MSK/ElastiCache/ECS all want."
}

variable "gateway_port" {
  type    = number
  default = 50051
}
