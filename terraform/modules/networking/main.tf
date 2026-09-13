# Networking module.
#
# STUDY NOTES: two public subnets across two AZs (real HA -- ECS/MSK/
# ElastiCache all want multi-AZ subnet placement to survive a single AZ
# failure), one VPC, one internet gateway. Public subnets (not
# public+private with a NAT gateway) are a deliberate simplification: a NAT
# gateway is a real recurring cost this portfolio deployment doesn't need
# to carry, and the gateway service itself needs inbound internet access
# anyway (devices connect to it directly). A production deployment
# fronting this with a load balancer and keeping the gateway itself in a
# private subnet would be the next hardening step -- named here, not done,
# consistent with this project's pattern of scoping what's built versus
# what's next.

resource "aws_vpc" "main" {
  cidr_block           = var.vpc_cidr
  enable_dns_support   = true
  enable_dns_hostnames = true
  tags = { Name = "${var.name_prefix}-vpc" }
}

resource "aws_internet_gateway" "main" {
  vpc_id = aws_vpc.main.id
  tags   = { Name = "${var.name_prefix}-igw" }
}

resource "aws_subnet" "public" {
  count                   = length(var.availability_zones)
  vpc_id                  = aws_vpc.main.id
  cidr_block              = cidrsubnet(var.vpc_cidr, 8, count.index)
  availability_zone       = var.availability_zones[count.index]
  map_public_ip_on_launch = true
  tags                    = { Name = "${var.name_prefix}-public-${var.availability_zones[count.index]}" }
}

resource "aws_route_table" "public" {
  vpc_id = aws_vpc.main.id
  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.main.id
  }
  tags = { Name = "${var.name_prefix}-public-rt" }
}

resource "aws_route_table_association" "public" {
  count          = length(aws_subnet.public)
  subnet_id      = aws_subnet.public[count.index].id
  route_table_id = aws_route_table.public.id
}

# Security group for the gateway itself: inbound gRPC (mTLS-protected at
# the application layer, per ADR-0011 -- this SG controls network reach,
# not identity) from anywhere devices might connect from.
resource "aws_security_group" "gateway" {
  name_prefix = "${var.name_prefix}-gateway-"
  vpc_id      = aws_vpc.main.id

  ingress {
    description = "gRPC (mTLS-protected at the application layer)"
    from_port   = var.gateway_port
    to_port     = var.gateway_port
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }
  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
  tags = { Name = "${var.name_prefix}-gateway-sg" }
}

# Security group for backing services (Redis, MSK) -- ingress restricted to
# the gateway's own security group, not a CIDR block, so only the gateway
# task can reach them regardless of what subnet it happens to land in.
resource "aws_security_group" "backing_services" {
  name_prefix = "${var.name_prefix}-backing-"
  vpc_id      = aws_vpc.main.id

  ingress {
    description     = "Redis"
    from_port       = 6379
    to_port         = 6379
    protocol        = "tcp"
    security_groups = [aws_security_group.gateway.id]
  }
  ingress {
    description     = "MSK Serverless (IAM-authenticated)"
    from_port       = 9098
    to_port         = 9098
    protocol        = "tcp"
    security_groups = [aws_security_group.gateway.id]
  }
  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
  tags = { Name = "${var.name_prefix}-backing-sg" }
}
