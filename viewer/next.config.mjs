/** @type {import('next').NextConfig} */
const nextConfig = {
  // Static export: `next build` writes viewer/out/, which CMake embeds into
  // the snowglobe binary. No server, no network at runtime (system fonts
  // only, no CDN, no telemetry — see .npmrc / build env).
  output: "export",
  images: { unoptimized: true },
  trailingSlash: false,
  poweredByHeader: false,
  reactStrictMode: false,
};

export default nextConfig;
