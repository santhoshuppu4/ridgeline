output "bootstrap_brokers_sasl_iam" {
  value = aws_msk_serverless_cluster.main.bootstrap_brokers_sasl_iam
}

output "cluster_arn" {
  value = aws_msk_serverless_cluster.main.arn
}
