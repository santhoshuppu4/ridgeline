output "vpc_id" {
  value = aws_vpc.main.id
}

output "public_subnet_ids" {
  value = aws_subnet.public[*].id
}

output "gateway_security_group_id" {
  value = aws_security_group.gateway.id
}

output "backing_services_security_group_id" {
  value = aws_security_group.backing_services.id
}
