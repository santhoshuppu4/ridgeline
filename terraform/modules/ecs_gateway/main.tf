# ECS Fargate module: runs the gateway container.
#
# WHAT THIS DOES NOT DO: build or push the gateway's Docker image. `image`
# is a variable (var.gateway_image) the caller must supply -- an ECR
# repository URI pointing at an image already built and pushed. Building
# that image (a real, non-trivial multi-stage Dockerfile compiling this
# project's C++ dependencies) and pushing it to ECR are manual steps
# documented in terraform/README.md, not automated here. Terraform
# provisioning infrastructure and a CI/CD pipeline building/pushing
# container images are two different concerns; conflating them here would
# make this module do too much and make neither piece easy to reason about
# on its own.
#
# IAM TASK ROLE IS SCOPED, NOT A BLANKET AWS-MANAGED POLICY: exactly
# dynamodb:GetItem/PutItem on the one table this gateway actually uses,
# exactly the MSK actions IAM-authenticated Kafka needs, exactly the S3
# actions needed to read (not write) OTA artifacts. A gateway compromise
# should not also hand over unrelated AWS permissions -- the same
# least-privilege instinct as the mTLS/tenant-identity work earlier in this
# project, applied to infrastructure instead of the application protocol.

resource "aws_ecs_cluster" "main" {
  name = "${var.name_prefix}-cluster"
}

resource "aws_cloudwatch_log_group" "gateway" {
  name              = "/ecs/${var.name_prefix}-gateway"
  retention_in_days = 14
}

data "aws_iam_policy_document" "ecs_assume_role" {
  statement {
    actions = ["sts:AssumeRole"]
    principals {
      type        = "Service"
      identifiers = ["ecs-tasks.amazonaws.com"]
    }
  }
}

resource "aws_iam_role" "execution" {
  name               = "${var.name_prefix}-gateway-execution"
  assume_role_policy = data.aws_iam_policy_document.ecs_assume_role.json
}

resource "aws_iam_role_policy_attachment" "execution" {
  role       = aws_iam_role.execution.name
  policy_arn = "arn:aws:iam::aws:policy/service-role/AmazonECSTaskExecutionRolePolicy"
}

resource "aws_iam_role" "task" {
  name               = "${var.name_prefix}-gateway-task"
  assume_role_policy = data.aws_iam_policy_document.ecs_assume_role.json
}

data "aws_iam_policy_document" "task_permissions" {
  statement {
    sid       = "DeviceShadowTable"
    actions   = ["dynamodb:GetItem", "dynamodb:PutItem"]
    resources = [var.dynamodb_table_arn]
  }
  statement {
    sid = "KafkaIamAuth"
    actions = [
      "kafka-cluster:Connect",
      "kafka-cluster:DescribeCluster",
      "kafka-cluster:WriteData",
      "kafka-cluster:DescribeTopic",
    ]
    resources = [var.msk_cluster_arn]
  }
  statement {
    sid       = "ReadOtaArtifacts"
    actions   = ["s3:GetObject"]
    resources = ["${var.ota_bucket_arn}/*"]
  }
}

resource "aws_iam_role_policy" "task" {
  name   = "${var.name_prefix}-gateway-task-permissions"
  role   = aws_iam_role.task.id
  policy = data.aws_iam_policy_document.task_permissions.json
}

resource "aws_ecs_task_definition" "gateway" {
  family                   = "${var.name_prefix}-gateway"
  requires_compatibilities = ["FARGATE"]
  network_mode             = "awsvpc"
  cpu                      = var.task_cpu
  memory                   = var.task_memory
  execution_role_arn       = aws_iam_role.execution.arn
  task_role_arn            = aws_iam_role.task.arn

  container_definitions = jsonencode([
    {
      name      = "gateway"
      image     = var.gateway_image
      essential = true
      portMappings = [
        { containerPort = var.gateway_port, protocol = "tcp" }
      ]
      logConfiguration = {
        logDriver = "awslogs"
        options = {
          "awslogs-group"         = aws_cloudwatch_log_group.gateway.name
          "awslogs-region"        = var.aws_region
          "awslogs-stream-prefix" = "gateway"
        }
      }
    }
  ])
}

resource "aws_ecs_service" "gateway" {
  name            = "${var.name_prefix}-gateway"
  cluster         = aws_ecs_cluster.main.id
  task_definition = aws_ecs_task_definition.gateway.arn
  desired_count   = var.desired_count
  launch_type     = "FARGATE"

  network_configuration {
    subnets          = var.subnet_ids
    security_groups  = [var.security_group_id]
    assign_public_ip = true  # Matches the networking module's public-subnet placement; see its own top comment.
  }
}
