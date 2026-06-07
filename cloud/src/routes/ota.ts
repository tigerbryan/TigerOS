import { Router } from "express";
import { z } from "zod";
import { prisma } from "../db.js";
import { requireDevice } from "../middleware/auth.js";

export const otaRouter = Router();

const otaCheckSchema = z.object({
  device_id: z.string().min(3),
  current_version: z.string().min(1),
  hardware_model: z.string().min(1),
  channel: z.enum(["stable", "beta"]).default("stable"),
});

function parseVersion(version: string): number[] {
  return version
    .trim()
    .replace(/^v/i, "")
    .split(/[.-]/)
    .map((part) => Number.parseInt(part, 10))
    .map((part) => (Number.isFinite(part) ? part : 0));
}

function compareVersions(a: string, b: string): number {
  const left = parseVersion(a);
  const right = parseVersion(b);
  const len = Math.max(left.length, right.length, 3);
  for (let i = 0; i < len; i += 1) {
    const diff = (left[i] || 0) - (right[i] || 0);
    if (diff !== 0) return diff;
  }
  return 0;
}

otaRouter.post("/check", requireDevice, async (req, res) => {
  const input = otaCheckSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid OTA check payload" });

  const releases = await prisma.firmwareRelease.findMany({
    where: {
      channel: input.data.channel,
      hardwareModel: input.data.hardware_model,
    },
  });
  const release = releases.sort((a, b) => compareVersions(b.version, a.version))[0];

  await prisma.device.update({
    where: { deviceId: input.data.device_id },
    data: {
      firmwareVersion: input.data.current_version,
      hardwareModel: input.data.hardware_model,
      channel: input.data.channel,
      online: true,
      lastSeen: new Date(),
    },
  });

  if (!release || compareVersions(release.version, input.data.current_version) <= 0) {
    return res.json({ update_available: false });
  }

  return res.json({
    update_available: true,
    version: release.version,
    firmware_url: release.firmwareUrl,
    sha256: release.sha256,
    release_notes: release.releaseNotes || "",
    force: release.forceUpdate,
  });
});
