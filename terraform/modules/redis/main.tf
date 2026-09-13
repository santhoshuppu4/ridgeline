# ElastiCache Redis module: the hot-state cache from ADR-0008.
#
# Single node (not a replication group with automatic failover): the data
# this cache holds is explicitly ephemeral by design (RedisHotStateStore's
# own header comment -- lost hot state is rebuilt from the next
# heartbeat), so paying for cross-AZ replica failover would be protecting
# data that's designed to be safely lossy in the first place. A real
# production deployment serving actual fire-detection alerts might
# reasonably choose differently; this is a deliberate, named cost/
# reliability tradeoff for a portfolio deployment, not an oversight.
#
# Engine version 7.x, matching the redis-server 7.0.15 the project's own
# tests run against (confirmed via `redis-server --version` during
# development) -- so behavior verified in tests matches what actually runs
# in this infrastructure, not a different major version with different
# defaults.

resource "aws_elasticache_subnet_group" "main" {
  name       = "${var.name_prefix}-redis-subnets"
  subnet_ids = var.subnet_ids
}

resource "aws_elasticache_cluster" "main" {
  cluster_id           = "${var.name_prefix}-redis"
  engine               = "redis"
  engine_version       = "7.1"
  node_type            = var.node_type
  num_cache_nodes      = 1
  port                 = 6379
  subnet_group_name    = aws_elasticache_subnet_group.main.name
  security_group_ids   = [var.security_group_id]
  apply_immediately    = true

  tags = { Name = "${var.name_prefix}-redis" }
}
