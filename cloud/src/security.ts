import bcrypt from "bcryptjs";
import jwt from "jsonwebtoken";
import { randomBytes } from "node:crypto";
import { config } from "./config.js";

export async function hashSecret(secret: string): Promise<string> {
  return bcrypt.hash(secret, 12);
}

export async function verifySecret(secret: string, hash: string): Promise<boolean> {
  return bcrypt.compare(secret, hash);
}

export function createJwt(userId: string): string {
  return jwt.sign({ sub: userId }, config.jwtSecret, { expiresIn: "7d" });
}

export function verifyJwt(token: string): { sub: string } {
  return jwt.verify(token, config.jwtSecret) as { sub: string };
}

export function generateDeviceToken(): string {
  return randomBytes(32).toString("hex");
}
