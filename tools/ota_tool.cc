// ridgeline_ota_tool: generate keys, sign a manifest, verify a manifest.
//
// This is the operator-facing wrapper around ridgeline_ota's library --
// the library is what's tested (tests/ota/ota_manifest_test.cc); this tool
// is a thin CLI so the signing workflow can actually be exercised by hand,
// not just from a test binary.
//
// Usage:
//   ridgeline_ota_tool genkey --out-prefix=/path/to/keys
//     -> writes /path/to/keys.private.pem and /path/to/keys.public.pem
//
//   ridgeline_ota_tool sign --private-key=keys.private.pem --version=42 --binary-sha256=<hex> --url=https://... --out=manifest.signed
//     -> writes a manifest + detached signature to --out
//
//   ridgeline_ota_tool verify --public-key=keys.public.pem --manifest=manifest.signed
//     -> exit 0 and prints OK if valid, exit 1 and prints FAIL otherwise

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "ridgeline/ota_manifest.h"
#include "ridgeline/time.h"

namespace {

std::string ReadFileOrDie(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(1); }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFileOrDie(const std::string& path, const std::string& contents) {
  std::ofstream out(path, std::ios::binary);
  if (!out) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); std::exit(1); }
  out << contents;
}

std::string ToHex(const std::string& bytes) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) { out.push_back(kHex[c >> 4]); out.push_back(kHex[c & 0xF]); }
  return out;
}

std::string FromHex(const std::string& hex) {
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

// The on-disk "signed manifest" format: one field per line, then the
// detached signature as the last line, hex-encoded. Simple, human-readable,
// and -- like the gateway's device-configs file -- deliberately not JSON,
// to avoid pulling nlohmann_json into a build config that doesn't already
// need it (RIDGELINE_WITH_DYNAMODB gates that dependency; ridgeline_ota
// doesn't require it).
std::string SerializeSignedManifest(const ridgeline::OtaManifest& m, const std::string& signature) {
  std::ostringstream out;
  out << "version=" << m.version << "\n"
      << "binary_sha256=" << m.binary_sha256_hex << "\n"
      << "url=" << m.url << "\n"
      << "issued_unix_ns=" << m.issued_unix_ns << "\n"
      << "signature_hex=" << ToHex(signature) << "\n";
  return out.str();
}

bool ParseSignedManifest(const std::string& text, ridgeline::OtaManifest& m, std::string& signature) {
  std::istringstream in(text);
  std::string line;
  auto value_after = [](const std::string& l, std::string_view key) -> std::string {
    return l.substr(key.size());
  };
  while (std::getline(in, line)) {
    if (line.rfind("version=", 0) == 0) m.version = std::strtoull(value_after(line, "version=").c_str(), nullptr, 10);
    else if (line.rfind("binary_sha256=", 0) == 0) m.binary_sha256_hex = value_after(line, "binary_sha256=");
    else if (line.rfind("url=", 0) == 0) m.url = value_after(line, "url=");
    else if (line.rfind("issued_unix_ns=", 0) == 0) m.issued_unix_ns = std::strtoll(value_after(line, "issued_unix_ns=").c_str(), nullptr, 10);
    else if (line.rfind("signature_hex=", 0) == 0) signature = FromHex(value_after(line, "signature_hex="));
    else if (!line.empty()) return false;
  }
  return !signature.empty();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s genkey|sign|verify [flags...]\n", argv[0]);
    return 2;
  }
  const std::string cmd = argv[1];

  std::string out_prefix, private_key_path, public_key_path, url, binary_sha256, out_path, manifest_path;
  std::uint64_t version = 0;
  for (int i = 2; i < argc; ++i) {
    std::string_view a{argv[i]};
    auto val = [&](std::string_view key) { return a.substr(0, key.size()) == key ? argv[i] + key.size() : nullptr; };
    if (auto v = val("--out-prefix=")) out_prefix = v;
    else if (auto v2 = val("--private-key=")) private_key_path = v2;
    else if (auto v3 = val("--public-key=")) public_key_path = v3;
    else if (auto v4 = val("--version=")) version = std::strtoull(v4, nullptr, 10);
    else if (auto v5 = val("--binary-sha256=")) binary_sha256 = v5;
    else if (auto v6 = val("--url=")) url = v6;
    else if (auto v7 = val("--out=")) out_path = v7;
    else if (auto v8 = val("--manifest=")) manifest_path = v8;
    else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
  }

  if (cmd == "genkey") {
    if (out_prefix.empty()) { std::fprintf(stderr, "genkey requires --out-prefix\n"); return 2; }
    const auto keys = ridgeline::GenerateOtaKeyPair();
    WriteFileOrDie(out_prefix + ".private.pem", keys.private_key_pem);
    WriteFileOrDie(out_prefix + ".public.pem", keys.public_key_pem);
    std::printf("wrote %s.private.pem and %s.public.pem\n", out_prefix.c_str(), out_prefix.c_str());
    std::printf("keep .private.pem secret; distribute .public.pem to every agent that must verify updates\n");
    return 0;
  }

  if (cmd == "sign") {
    if (private_key_path.empty() || binary_sha256.empty() || url.empty() || out_path.empty()) {
      std::fprintf(stderr, "sign requires --private-key --version --binary-sha256 --url --out\n");
      return 2;
    }
    ridgeline::OtaManifest m;
    m.version = version;
    m.binary_sha256_hex = binary_sha256;
    m.url = url;
    m.issued_unix_ns = ridgeline::NowUnixNs();
    const std::string sig = ridgeline::SignManifest(m, ReadFileOrDie(private_key_path));
    WriteFileOrDie(out_path, SerializeSignedManifest(m, sig));
    std::printf("signed manifest version=%llu written to %s\n", static_cast<unsigned long long>(version), out_path.c_str());
    return 0;
  }

  if (cmd == "verify") {
    if (public_key_path.empty() || manifest_path.empty()) {
      std::fprintf(stderr, "verify requires --public-key --manifest\n");
      return 2;
    }
    ridgeline::OtaManifest m;
    std::string signature;
    if (!ParseSignedManifest(ReadFileOrDie(manifest_path), m, signature)) {
      std::printf("FAIL: could not parse %s\n", manifest_path.c_str());
      return 1;
    }
    const bool ok = ridgeline::VerifyManifestSignature(m, signature, ReadFileOrDie(public_key_path));
    std::printf("%s: version=%llu binary_sha256=%s url=%s\n", ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(m.version), m.binary_sha256_hex.c_str(), m.url.c_str());
    return ok ? 0 : 1;
  }

  std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  return 2;
}
