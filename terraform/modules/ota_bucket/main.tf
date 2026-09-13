# S3 bucket for hosting signed OTA manifests and binaries (ADR-0012).
#
# Private by default, public access explicitly blocked: devices in a real
# deployment would need pre-signed URLs (or a CloudFront distribution
# fronting the bucket with signed cookies/URLs) to fetch update artifacts,
# not direct public HTTP access. Generating and distributing those signed
# URLs, and the CloudFront distribution itself, are real additional pieces
# not built here -- named as a following step, not silently assumed.
#
# Versioning enabled: an OTA manifest and its corresponding binary should
# never be silently overwritten in place. Each signed manifest already
# carries its own `version` field (ota/include/ridgeline/ota_manifest.h);
# S3 versioning is a second, independent safety net at the storage layer
# itself, not a replacement for that application-level version field.

resource "aws_s3_bucket" "ota" {
  bucket = "${var.name_prefix}-ota-artifacts"
  tags   = { Name = "${var.name_prefix}-ota-artifacts" }
}

resource "aws_s3_bucket_versioning" "ota" {
  bucket = aws_s3_bucket.ota.id
  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_public_access_block" "ota" {
  bucket                  = aws_s3_bucket.ota.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_server_side_encryption_configuration" "ota" {
  bucket = aws_s3_bucket.ota.id
  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}
