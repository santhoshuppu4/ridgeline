# MSK Serverless module: the real production equivalent of the Redpanda
# container used in local dev (deploy/docker-compose.yml).
#
# WHY SERVERLESS, NOT A PROVISIONED MSK CLUSTER: no broker sizing or
# partition-count capacity planning to get right up front, and cost scales
# with actual throughput rather than a fixed always-on broker fleet -- the
# right tradeoff for a portfolio deployment's genuinely low and unpredictable
# traffic. A production deployment at real fleet scale (see ADR-0009's
# thread-count findings) would likely revisit this in favor of provisioned
# MSK once traffic is well-characterized, the same way ADR-0009 said the
# gateway's own threading model should be revisited at scale rather than
# guessed at up front.
#
# IAM-based client authentication (not SASL/SCRAM or mutual TLS between
# the gateway and the broker): the ECS task's own IAM role is the
# credential, so there's no separate broker-level secret to provision,
# rotate, or leak -- consistent with this project's existing pattern of
# preferring an already-established identity mechanism over inventing a
# parallel one (mTLS device identity in ADR-0011 reused gRPC's own
# handshake rather than a bolted-on token scheme).

resource "aws_msk_serverless_cluster" "main" {
  cluster_name = "${var.name_prefix}-kafka"

  vpc_config {
    subnet_ids         = var.subnet_ids
    security_group_ids = [var.security_group_id]
  }

  client_authentication {
    sasl {
      iam {
        enabled = true
      }
    }
  }

  tags = { Name = "${var.name_prefix}-kafka" }
}
