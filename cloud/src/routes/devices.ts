import { Router } from "express";
import { z } from "zod";
import { prisma } from "../db.js";
import { AuthRequest, requireDevice, requireUser } from "../middleware/auth.js";
import { generateDeviceToken, hashSecret } from "../security.js";

export const devicesRouter = Router();

const registerSchema = z.object({
  device_id: z.string().min(3),
  name: z.string().optional(),
  hardware_model: z.string().min(1),
  firmware_version: z.string().optional(),
  channel: z.enum(["stable", "beta"]).default("stable"),
});

devicesRouter.post("/register", requireUser, async (req: AuthRequest, res) => {
  const input = registerSchema.safeParse(req.body);
  if (!input.success || !req.userId) return res.status(400).json({ ok: false, error: "Invalid device registration payload" });

  const existing = await prisma.device.findUnique({ where: { deviceId: input.data.device_id } });
  if (existing && existing.ownerId !== req.userId) {
    return res.status(409).json({ ok: false, error: "Device is already registered to another user" });
  }

  const token = generateDeviceToken();
  const data = {
    name: input.data.name,
    hardwareModel: input.data.hardware_model,
    firmwareVersion: input.data.firmware_version,
    channel: input.data.channel,
    tokenHash: await hashSecret(token),
  };
  const device = existing
    ? await prisma.device.update({
        where: { deviceId: input.data.device_id },
        data,
      })
    : await prisma.device.create({
        data: {
          ...data,
          deviceId: input.data.device_id,
          ownerId: req.userId,
        },
      });
  return res.status(201).json({ ok: true, device, device_token: token });
});

devicesRouter.get("/", requireUser, async (req: AuthRequest, res) => {
  const devices = await prisma.device.findMany({
    where: { ownerId: req.userId },
    orderBy: { updatedAt: "desc" },
  });
  return res.json({ ok: true, devices });
});

devicesRouter.get("/:deviceId", requireUser, async (req: AuthRequest, res) => {
  const deviceId = String(req.params.deviceId);
  const device = await prisma.device.findFirst({
    where: { ownerId: req.userId, deviceId },
  });
  if (!device) return res.status(404).json({ ok: false, error: "Device not found" });
  return res.json({ ok: true, device });
});

const heartbeatSchema = z.object({
  device_id: z.string(),
  firmware_version: z.string().optional(),
  ip: z.string().optional(),
  rssi: z.number().int().optional(),
  free_heap: z.number().int().optional(),
  uptime: z.number().int().optional(),
});

devicesRouter.post("/heartbeat", requireDevice, async (req: AuthRequest, res) => {
  const input = heartbeatSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid heartbeat payload" });

  const device = await prisma.device.update({
    where: { deviceId: input.data.device_id },
    data: {
      firmwareVersion: input.data.firmware_version,
      ipAddress: input.data.ip,
      rssi: input.data.rssi,
      freeHeap: input.data.free_heap,
      uptime: input.data.uptime,
      online: true,
      lastSeen: new Date(),
    },
  });
  return res.json({ ok: true, device });
});
