import type { NextFunction, Request, Response } from "express";
import { prisma } from "../db.js";
import { verifyJwt, verifySecret } from "../security.js";

export type AuthRequest = Request & {
  userId?: string;
  device?: {
    id: string;
    deviceId: string;
  };
};

function bearer(req: Request): string | null {
  const header = req.header("authorization") || "";
  const [scheme, token] = header.split(" ");
  return scheme?.toLowerCase() === "bearer" && token ? token : null;
}

export function requireUser(req: AuthRequest, res: Response, next: NextFunction) {
  const token = bearer(req);
  if (!token) return res.status(401).json({ ok: false, error: "JWT bearer token required" });
  try {
    const payload = verifyJwt(token);
    req.userId = payload.sub;
    return next();
  } catch {
    return res.status(401).json({ ok: false, error: "Invalid JWT" });
  }
}

export async function requireDevice(req: AuthRequest, res: Response, next: NextFunction) {
  const token = bearer(req);
  const deviceId = req.body?.device_id || req.body?.deviceId || req.params?.gateway_id || req.params?.gatewayId;
  if (!token || !deviceId) {
    return res.status(401).json({ ok: false, error: "Device token and device_id required" });
  }

  const device = await prisma.device.findUnique({ where: { deviceId: String(deviceId) } });
  if (!device || !(await verifySecret(token, device.tokenHash))) {
    return res.status(401).json({ ok: false, error: "Invalid device token" });
  }
  req.device = { id: device.id, deviceId: device.deviceId };
  return next();
}
