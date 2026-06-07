import { Router } from "express";
import { z } from "zod";
import { prisma } from "../db.js";
import { createJwt, hashSecret, verifySecret } from "../security.js";

export const authRouter = Router();

const credentialsSchema = z.object({
  email: z.string().email(),
  password: z.string().min(8),
  name: z.string().optional(),
});

authRouter.post("/register", async (req, res) => {
  const input = credentialsSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid registration payload" });

  const user = await prisma.user.create({
    data: {
      email: input.data.email.toLowerCase(),
      name: input.data.name,
      passwordHash: await hashSecret(input.data.password),
    },
  });
  return res.status(201).json({ ok: true, token: createJwt(user.id), user: { id: user.id, email: user.email, name: user.name } });
});

authRouter.post("/login", async (req, res) => {
  const input = credentialsSchema.pick({ email: true, password: true }).safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid login payload" });

  const user = await prisma.user.findUnique({ where: { email: input.data.email.toLowerCase() } });
  if (!user || !(await verifySecret(input.data.password, user.passwordHash))) {
    return res.status(401).json({ ok: false, error: "Invalid email or password" });
  }
  return res.json({ ok: true, token: createJwt(user.id), user: { id: user.id, email: user.email, name: user.name } });
});
