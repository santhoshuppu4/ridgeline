variable "name_prefix" {
  type = string
}

variable "table_name" {
  type        = string
  default     = "device_shadows"  # Matches DynamoDbConfig::table_name's own default in dynamodb/include/ridgeline/device_shadow_store.h.
  description = "Must match the --dynamodb-table gateway flag, or the table won't be the one the gateway actually reads/writes."
}
