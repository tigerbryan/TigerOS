import "dotenv/config";

const jwtSecret = process.env.JWT_SECRET || "";
if (process.env.NODE_ENV === "production" && jwtSecret.length < 32) {
  throw new Error("JWT_SECRET must be set to at least 32 characters in production");
}

export const config = {
  port: Number(process.env.PORT || 8080),
  jwtSecret: jwtSecret || "dev-only-change-me",
  corsOrigin: process.env.CORS_ORIGIN || "*",
};
