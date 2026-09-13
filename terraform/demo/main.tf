# Live demo stack -- deliberately separate from ../main.tf (the full
# Kafka/Redis/ECS infrastructure, which has real recurring cost). Every
# resource here is chosen specifically to stay on AWS's Always Free tier
# (Lambda, DynamoDB) or its 12-months-free tier (API Gateway HTTP API) --
# see demo/README.md for the exact cost reasoning per service, and the
# budget alert below as a hard backstop rather than just an assurance.

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

# ---------------------------------------------------------------------------
# DynamoDB: the demo event history. On-demand billing, no capacity to
# provision or overpay for -- and this table alone never leaves the
# permanent Always Free tier (25GB storage, 25 RCU/WCU-equivalent) at any
# realistic portfolio-demo volume.
# ---------------------------------------------------------------------------
resource "aws_dynamodb_table" "demo_events" {
  name         = "${var.name_prefix}-demo-events"
  billing_mode = "PAY_PER_REQUEST"
  hash_key     = "event_id"

  attribute {
    name = "event_id"
    type = "S"
  }

  tags = { Name = "${var.name_prefix}-demo-events" }
}

# ---------------------------------------------------------------------------
# ECR repository for the Lambda container image. Built and pushed
# manually -- see demo/README.md -- not by Terraform itself, same reasoning
# as the main gateway image in ../modules/ecs_gateway.
# ---------------------------------------------------------------------------
resource "aws_ecr_repository" "demo_lambda" {
  name         = "${var.name_prefix}-demo-lambda"
  force_delete = true  # A portfolio demo repo -- fine to delete along with any images in it on teardown.
}

# ---------------------------------------------------------------------------
# IAM: scoped to exactly what the Lambda needs -- dynamodb:PutItem/Scan on
# this one table, and the three logging actions AWS's own managed policy
# would grant anyway. No broader AWSLambdaBasicExecutionRole attachment:
# that managed policy is scoped correctly for logging but this makes the
# DynamoDB scope explicit rather than bundling it into a separate,
# easy-to-overlook second policy.
# ---------------------------------------------------------------------------
data "aws_iam_policy_document" "lambda_assume_role" {
  statement {
    actions = ["sts:AssumeRole"]
    principals {
      type        = "Service"
      identifiers = ["lambda.amazonaws.com"]
    }
  }
}

resource "aws_iam_role" "lambda_exec" {
  name               = "${var.name_prefix}-demo-lambda-exec"
  assume_role_policy = data.aws_iam_policy_document.lambda_assume_role.json
}

resource "aws_cloudwatch_log_group" "demo_lambda_logs" {
  name              = "/aws/lambda/${var.name_prefix}-demo"
  retention_in_days = 7  # Short retention: this is a demo, not a system whose logs need long-term retention.
}

data "aws_iam_policy_document" "lambda_permissions" {
  statement {
    sid       = "DemoEventsTable"
    actions   = ["dynamodb:PutItem", "dynamodb:Scan"]
    resources = [aws_dynamodb_table.demo_events.arn]
  }
  statement {
    sid       = "Logging"
    actions   = ["logs:CreateLogStream", "logs:PutLogEvents"]
    resources = ["${aws_cloudwatch_log_group.demo_lambda_logs.arn}:*"]
  }
}

resource "aws_iam_role_policy" "lambda_exec" {
  name   = "${var.name_prefix}-demo-lambda-permissions"
  role   = aws_iam_role.lambda_exec.id
  policy = data.aws_iam_policy_document.lambda_permissions.json
}

# ---------------------------------------------------------------------------
# Lambda function. Container-packaged (see Dockerfile.demo-lambda at the
# repo root) so this runs the ACTUAL compiled AlertEngine/WeatherClient
# code, not a reimplementation in a scripting-language Lambda runtime.
# ---------------------------------------------------------------------------
resource "aws_lambda_function" "demo" {
  function_name = "${var.name_prefix}-demo"
  role          = aws_iam_role.lambda_exec.arn
  package_type  = "Image"
  image_uri     = var.lambda_image_uri
  timeout       = 10   # A real weather API round trip plus a DynamoDB write can take a few seconds; 10s gives headroom.
  memory_size   = 256  # Modest: this handler does a couple of HTTP calls and light JSON work, not heavy compute.

  environment {
    variables = {
      DEMO_TABLE_NAME = aws_dynamodb_table.demo_events.name
    }
  }

  depends_on = [aws_cloudwatch_log_group.demo_lambda_logs]
}

# ---------------------------------------------------------------------------
# API Gateway HTTP API (not a REST API): HTTP APIs are the cheaper of the
# two API Gateway types and are all this demo needs (a proxy integration to
# one Lambda, two routes, no request/response transformation).
# ---------------------------------------------------------------------------
resource "aws_apigatewayv2_api" "demo" {
  name          = "${var.name_prefix}-demo-api"
  protocol_type = "HTTP"
  cors_configuration {
    allow_origins = ["*"]  # A public portfolio demo: intentionally open, not serving anything sensitive.
    allow_methods = ["GET", "POST", "OPTIONS"]
    allow_headers = ["content-type"]
  }
}

resource "aws_apigatewayv2_integration" "demo_lambda" {
  api_id                 = aws_apigatewayv2_api.demo.id
  integration_type       = "AWS_PROXY"
  integration_uri        = aws_lambda_function.demo.invoke_arn
  payload_format_version = "2.0"
}

resource "aws_apigatewayv2_route" "list_events" {
  api_id    = aws_apigatewayv2_api.demo.id
  route_key = "GET /events"
  target    = "integrations/${aws_apigatewayv2_integration.demo_lambda.id}"
}

resource "aws_apigatewayv2_route" "trigger" {
  api_id    = aws_apigatewayv2_api.demo.id
  route_key = "POST /trigger"
  target    = "integrations/${aws_apigatewayv2_integration.demo_lambda.id}"
}

resource "aws_apigatewayv2_stage" "default" {
  api_id      = aws_apigatewayv2_api.demo.id
  name        = "$default"
  auto_deploy = true
}

resource "aws_lambda_permission" "apigw" {
  statement_id  = "AllowAPIGatewayInvoke"
  action        = "lambda:InvokeFunction"
  function_name = aws_lambda_function.demo.function_name
  principal     = "apigateway.amazonaws.com"
  source_arn    = "${aws_apigatewayv2_api.demo.execution_arn}/*/*"
}

# ---------------------------------------------------------------------------
# EventBridge: the "live" synthetic traffic. Every 5 minutes -- frequent
# enough that a visitor watching the page for a few minutes sees the feed
# actually move, infrequent enough that this is a handful of Lambda
# invocations per hour, nowhere near any billing threshold.
# ---------------------------------------------------------------------------
resource "aws_cloudwatch_event_rule" "scheduled_traffic" {
  name                = "${var.name_prefix}-demo-scheduled-traffic"
  schedule_expression = "rate(5 minutes)"
}

resource "aws_cloudwatch_event_target" "scheduled_traffic" {
  rule = aws_cloudwatch_event_rule.scheduled_traffic.name
  arn  = aws_lambda_function.demo.arn
}

resource "aws_lambda_permission" "eventbridge" {
  statement_id  = "AllowEventBridgeInvoke"
  action        = "lambda:InvokeFunction"
  function_name = aws_lambda_function.demo.function_name
  principal     = "events.amazonaws.com"
  source_arn    = aws_cloudwatch_event_rule.scheduled_traffic.arn
}

# ---------------------------------------------------------------------------
# Cost backstop: a hard budget alert. NOTE this monitors the WHOLE AWS
# ACCOUNT's spend, not just this demo stack specifically -- accurately
# scoping a budget to one small set of resources needs cost allocation
# tags configured well in advance, which is more setup than a portfolio
# demo justifies. A blunt "email me if my account's bill this month
# exceeds $1" is the honest, practical safety net here, not a
# resource-scoped guarantee.
# ---------------------------------------------------------------------------
resource "aws_budgets_budget" "cost_alert" {
  name         = "${var.name_prefix}-demo-cost-alert"
  budget_type  = "COST"
  limit_amount = "1"
  limit_unit   = "USD"
  time_unit    = "MONTHLY"

  notification {
    comparison_operator        = "GREATER_THAN"
    threshold                  = 100
    threshold_type             = "PERCENTAGE"
    notification_type          = "ACTUAL"
    subscriber_email_addresses = [var.alert_email]
  }
}
