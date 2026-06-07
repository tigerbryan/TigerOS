import { Router } from "express";
import { z } from "zod";
import { prisma } from "../db.js";
import { requireUser } from "../middleware/auth.js";

export const firmwareRouter = Router();

const releaseSchema = z.object({
  version: z.string().min(1),
  channel: z.enum(["stable", "beta"]),
  hardware_model: z.string().min(1),
  firmware_url: z.string().url(),
  sha256: z.string().regex(/^[a-fA-F0-9]{64}$/),
  release_notes: z.string().optional(),
  force_update: z.boolean().default(false),
});

firmwareRouter.post("/releases", requireUser, async (req, res) => {
  const input = releaseSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid firmware release payload" });

  const release = await prisma.firmwareRelease.create({
    data: {
      version: input.data.version,
      channel: input.data.channel,
      hardwareModel: input.data.hardware_model,
      firmwareUrl: input.data.firmware_url,
      sha256: input.data.sha256.toLowerCase(),
      releaseNotes: input.data.release_notes,
      forceUpdate: input.data.force_update,
    },
  });
  return res.status(201).json({ ok: true, release, upload: { placeholder: true, message: "Binary upload storage is not implemented in V0.8." } });
});

firmwareRouter.get("/releases", requireUser, async (_req, res) => {
  const releases = await prisma.firmwareRelease.findMany({ orderBy: { createdAt: "desc" } });
  return res.json({ ok: true, releases });
});
