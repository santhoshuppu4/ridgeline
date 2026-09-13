# Ridgeline AWS infrastructure

Provisions the production backing services for the gateway: DynamoDB
(device shadows), ElastiCache Redis (hot state), MSK Serverless (Kafka
event backbone), an S3 bucket (signed OTA artifacts), and an ECS Fargate
service running the gateway itself.

## Before you run anything here

**This configuration has not been applied against a real AWS account.**
It was validated with `terraform-config-inspect` (real syntax/reference
checking, no AWS credentials needed) and by manually cross-checking every
variable passed between the root module and each submodule. It has NOT
been checked with `terraform validate` or `terraform plan` against the
real AWS provider, and has never run `terraform apply`. See
`context/adr/0015-terraform-aws-infrastructure.md` for exactly why, and
run both of those yourself before trusting this in production.

## A real integration gap, found while writing this

**`ridgeline_gateway`'s existing `KafkaProducer` (from ADR-0007) cannot
actually authenticate to the MSK Serverless cluster this Terraform
provisions.** MSK Serverless requires AWS IAM SASL authentication;
`KafkaProducer` was built and tested against plaintext broker connections
only (a real mock cluster, then a real local Redpanda broker -- see
ADR-0007), with no IAM SASL support. Provisioning MSK Serverless with IAM
auth was the right infrastructure choice on its own terms (see the
kafka module's comments for why), but it means **the gateway cannot
connect to this specific Kafka cluster as built today.** Closing this gap
needs one of:
- Adding AWS IAM SASL support to `KafkaProducer` (librdkafka does support
  this via the `aws-msk-iam-sasl-signer` mechanism, but it's not
  implemented here), or
- Switching this Terraform to a provisioned MSK cluster with
  unauthenticated access restricted to the VPC's security groups alone
  (matching what `KafkaProducer` actually supports today).

This is named here deliberately rather than discovered the first time
someone tries to actually connect the two.

## Deployment workflow

1. **Build and push the gateway image** (not automated by Terraform --
   see `Dockerfile.gateway` at the repo root):
   ```bash
   aws ecr create-repository --repository-name ridgeline-gateway
   docker build -f Dockerfile.gateway -t ridgeline-gateway .
   aws ecr get-login-password | docker login --username AWS --password-stdin <account-id>.dkr.ecr.<region>.amazonaws.com
   docker tag ridgeline-gateway:latest <account-id>.dkr.ecr.<region>.amazonaws.com/ridgeline-gateway:latest
   docker push <account-id>.dkr.ecr.<region>.amazonaws.com/ridgeline-gateway:latest
   ```
   `Dockerfile.gateway` was written and reasoned about but never actually
   built in this project's development sandbox -- confirm it builds
   cleanly before relying on it.

2. **Initialize and plan:**
   ```bash
   cd terraform
   terraform init
   terraform plan -var="gateway_image=<account-id>.dkr.ecr.<region>.amazonaws.com/ridgeline-gateway:latest"
   ```
   Read the plan output carefully -- this is the first real semantic
   validation this configuration will have ever had.

3. **Apply:**
   ```bash
   terraform apply -var="gateway_image=..."
   ```

4. **Find the gateway's address** (not a stable Terraform output -- see
   `outputs.tf`'s note on why):
   ```bash
   aws ecs list-tasks --cluster $(terraform output -raw ecs_cluster_name)
   aws ecs describe-tasks --cluster ... --tasks ... | grep -A2 networkInterfaceId
   ```

5. **Tear down** when done, to stop incurring cost:
   ```bash
   terraform destroy -var="gateway_image=..."
   ```

## What's deliberately not built here

- No load balancer or stable DNS name for the gateway -- devices would
  need a fixed address in any real deployment; this configuration doesn't
  provide one.
- No NAT gateway / private subnets -- the gateway runs in a public subnet
  directly, a real cost tradeoff explained in `modules/networking/main.tf`.
- No CloudFront distribution for signed OTA artifact URLs -- the S3
  bucket is provisioned but private; generating and distributing signed
  URLs is a separate piece.
- No CI/CD pipeline building and pushing the gateway image automatically.
