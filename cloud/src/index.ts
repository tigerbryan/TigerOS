import cors from "cors";
import express from "express";
import helmet from "helmet";
import morgan from "morgan";
import { config } from "./config.js";
import { authRouter } from "./routes/auth.js";
import { devicesRouter } from "./routes/devices.js";
import { firmwareRouter } from "./routes/firmware.js";
import { gatewaysRouter } from "./routes/gateways.js";
import { otaRouter } from "./routes/ota.js";

const app = express();

app.use(helmet());
app.use(cors({ origin: config.corsOrigin }));
app.use(express.json({ limit: "1mb" }));
app.use(morgan("combined"));

app.get("/health", (_req, res) => {
  res.json({ ok: true, service: "tigeros-cloud", version: "1.0.0" });
});

app.use("/api/auth", authRouter);
app.use("/api/devices", devicesRouter);
app.use("/api/firmware", firmwareRouter);
app.use("/api/gateways", gatewaysRouter);
app.use("/api/ota", otaRouter);

app.use((req, res) => {
  res.status(404).json({ ok: false, error: `Route not found: ${req.method} ${req.path}` });
});

app.use((error: unknown, _req: express.Request, res: express.Response, _next: express.NextFunction) => {
  console.error(error instanceof Error ? error.message : "Unhandled error");
  res.status(500).json({ ok: false, error: "Internal server error" });
});

app.listen(config.port, () => {
  console.log(`TigerOS Cloud listening on :${config.port}`);
});
