variable "name_prefix" {
  type = string
}

variable "subnet_ids" {
  type = list(string)
}

variable "security_group_id" {
  type = string
}

variable "node_type" {
  type        = string
  default     = "cache.t4g.micro"  # Smallest current-generation ARM (Graviton) node -- cheapest real option, not a placeholder.
  description = "ElastiCache node type. cache.t4g.micro is a genuinely deployable choice for a low-traffic portfolio deployment."
}
