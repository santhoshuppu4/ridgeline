# Ridgeline — live demo

A real, live-running slice of Ridgeline: your actual compiled `AlertEngine`
and `WeatherClient` code (from `weather/`), running in AWS Lambda, fed by a
scheduled synthetic feed and a visitor-triggerable "try it yourself" button
on a free static webpage.

**Scope, stated plainly:** this exercises the alert-fusion logic
(ADR-0014), not the full ONNX object-detection pipeline. Running real-time
inference in a public-facing Lambda at zero guaranteed cost isn't a
realistic combination; this demo is honest about testing the part of the
system that *can* run indefinitely for free.

## Before you deploy: the budget alert (do this first)

```bash
aws budgets create-budget \
  --account-id <your-account-id> \
  --budget file://budget.json \
  --notifications-with-subscribers file://notifications.json
```

Or, since this is already in `terraform/demo/main.tf` as
`aws_budgets_budget.cost_alert`, it gets created automatically when you
`terraform apply` below — just make sure `var.alert_email` is a real
address you check. **This monitors your whole AWS account's spend, not
just this demo** (see the resource's own comment in `main.tf` for why a
tighter scope needs more setup than a portfolio demo justifies) — a blunt
but real safety net.

## What's genuinely free here, and what isn't (12 months only)

| Resource | Tier |
|---|---|
| Lambda | Always Free (1M requests/month, permanent) |
| DynamoDB | Always Free (25GB, permanent) |
| API Gateway HTTP API | Free for 12 months from account creation, then ~$1/million requests |
| EventBridge scheduled rule | Always Free |
| ECR (small image, low pull count) | A few cents/month at most after any free allowance |

At real portfolio-demo traffic (a handful of viewers, a few hundred
requests a month at most), the non-permanent items round to effectively
$0 — but "effectively" is why the budget alert above exists.

## Deploy steps

**1. Build and push the Lambda container image:**
```bash
cd ~/code/ridgeline_v2
aws ecr get-login-password --region us-west-2 | docker login --username AWS --password-stdin <account-id>.dkr.ecr.us-west-2.amazonaws.com
docker build -f Dockerfile.demo-lambda -t ridgeline-demo-lambda .
```
`Dockerfile.demo-lambda` was written and reasoned about but never actually
built in this project's development sandbox (no Docker daemon there, and
`amazonlinux:2023`/`public.ecr.aws` base images aren't reachable from it
either) — confirm it builds cleanly before relying on it. What WAS
verified directly: the handler binary it packages compiles and links
successfully against the real `aws-lambda-cpp` library (see
`context/adr/0016-live-demo.md`).

**2. Create the ECR repo first (or apply Terraform once to create it, then come back for the image):**
```bash
cd terraform/demo
terraform init
terraform apply -target=aws_ecr_repository.demo_lambda -var="lambda_image_uri=placeholder" -var="alert_email=you@example.com"
```

**3. Push the image:**
```bash
docker tag ridgeline-demo-lambda:latest <account-id>.dkr.ecr.us-west-2.amazonaws.com/ridgeline-demo-lambda:latest
docker push <account-id>.dkr.ecr.us-west-2.amazonaws.com/ridgeline-demo-lambda:latest
```

**4. Apply the full stack:**
```bash
terraform apply \
  -var="lambda_image_uri=<account-id>.dkr.ecr.us-west-2.amazonaws.com/ridgeline-demo-lambda:latest" \
  -var="alert_email=you@example.com"
```

**5. Get the API endpoint:**
```bash
terraform output api_endpoint
```

**6. Wire the frontend to it:**
Edit `demo/frontend/index.html`, replace `REPLACE_WITH_YOUR_API_ENDPOINT`
with the URL from step 5.

**7. Publish the frontend on GitHub Pages (free, permanent):**
```bash
git add demo/frontend/index.html
git commit -m "Point demo frontend at deployed API endpoint"
git push
```
Then in the GitHub repo settings → Pages → deploy from `main` branch,
`/demo/frontend` folder (or copy `index.html` to repo root / a `docs/`
folder, whichever your Pages configuration expects).

## Smoke test after deploying

```bash
curl -X POST "$(terraform output -raw api_endpoint)/trigger" -H "Content-Type: application/json" -d '{"confidence": 0.9}'
curl "$(terraform output -raw api_endpoint)/events"
```
The first call's `weather_available` field is the real proof this is
working end to end: `true` means a genuine live weather fetch inside
Lambda succeeded; `false` (with a still-valid, confidence-only severity)
means the graceful-degradation path fired instead — both are correct
behavior, and either way you'll know immediately which one happened.

## Tearing down

```bash
cd terraform/demo
terraform destroy -var="lambda_image_uri=..." -var="alert_email=..."
```
Deletes everything, including the ECR repo and its images
(`force_delete = true`).
