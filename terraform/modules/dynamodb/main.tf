# DynamoDB module: the device shadow table, schema matched EXACTLY to what
# ridgeline/device_shadow_store.h already expects -- partition key
# "device_id" (string), table name "device_shadows" by default (the
# DynamoDbConfig struct's own default). This is deliberate: infrastructure
# defined here that doesn't match the application code's real expectations
# would be a Terraform config that "looks right" but silently breaks the
# first time it's actually used, which is exactly the kind of gap this
# whole project has tried to avoid at every phase.
#
# Billing mode PAY_PER_REQUEST (on-demand), not provisioned capacity: no
# capacity planning needed, and a portfolio deployment's actual traffic is
# far too low and unpredictable to benefit from provisioned throughput's
# lower per-request cost at volume.

resource "aws_dynamodb_table" "device_shadows" {
  name         = var.table_name
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "device_id"

  attribute {
    name = "device_id"
    type = "S"
  }

  point_in_time_recovery {
    enabled = true
  }

  server_side_encryption {
    enabled = true
  }

  tags = { Name = "${var.name_prefix}-device-shadows" }
}
