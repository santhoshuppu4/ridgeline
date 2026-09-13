output "dynamodb_table_name" {
  value = module.dynamodb.table_name
}

output "redis_endpoint" {
  value = "${module.redis.endpoint}:${module.redis.port}"
}

output "kafka_bootstrap_brokers_sasl_iam" {
  value = module.kafka.bootstrap_brokers_sasl_iam
}

output "ota_bucket_name" {
  value = module.ota_bucket.bucket_name
}

output "ecs_cluster_name" {
  value = module.ecs_gateway.cluster_name
}

output "ecs_service_name" {
  value = module.ecs_gateway.service_name
}

# NOT an output: the gateway's own public IP/DNS. ECS Fargate with
# assign_public_ip=true gets a new public IP on every task restart unless
# fronted by a load balancer or Elastic IP -- neither of which this
# configuration provisions (see terraform/README.md's "what's not built
# here" section). Look this up via `aws ecs describe-tasks` after deploying,
# don't rely on it being stable.
