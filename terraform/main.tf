# Root module: wires networking, DynamoDB, Redis, MSK, the OTA bucket, and
# the ECS gateway service together. See terraform/README.md before running
# anything here -- in particular, the gateway's container image must be
# built and pushed to ECR manually first (var.gateway_image has no
# default), and this configuration has NOT been applied against a real AWS
# account (see context/adr/0015 for exactly what was and wasn't verified,
# and why).

terraform {
  required_version = ">= 1.5"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

provider "aws" {
  region = var.aws_region
}

module "networking" {
  source             = "./modules/networking"
  name_prefix        = var.name_prefix
  availability_zones = var.availability_zones
  gateway_port       = var.gateway_port
}

module "dynamodb" {
  source      = "./modules/dynamodb"
  name_prefix = var.name_prefix
  table_name  = var.dynamodb_table_name
}

module "redis" {
  source             = "./modules/redis"
  name_prefix        = var.name_prefix
  subnet_ids         = module.networking.public_subnet_ids
  security_group_id  = module.networking.backing_services_security_group_id
  node_type          = var.redis_node_type
}

module "kafka" {
  source             = "./modules/kafka"
  name_prefix        = var.name_prefix
  subnet_ids         = module.networking.public_subnet_ids
  security_group_id  = module.networking.backing_services_security_group_id
}

module "ota_bucket" {
  source      = "./modules/ota_bucket"
  name_prefix = var.name_prefix
}

module "ecs_gateway" {
  source              = "./modules/ecs_gateway"
  name_prefix         = var.name_prefix
  aws_region          = var.aws_region
  subnet_ids          = module.networking.public_subnet_ids
  security_group_id   = module.networking.gateway_security_group_id
  gateway_image       = var.gateway_image
  gateway_port        = var.gateway_port
  dynamodb_table_arn  = module.dynamodb.table_arn
  msk_cluster_arn      = module.kafka.cluster_arn
  ota_bucket_arn      = module.ota_bucket.bucket_arn
}
