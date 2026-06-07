import { Router } from "express";
import type { Prisma } from "@prisma/client";
import { z } from "zod";
import { prisma } from "../db.js";
import { AuthRequest, requireDevice, requireUser } from "../middleware/auth.js";

export const gatewaysRouter = Router();

const sensorSchema = z.object({
  mac: z.string().min(3),
  name: z.string().optional(),
  brand: z.string().optional(),
  model: z.string().optional(),
  protocol: z.string().optional(),
  sensor_type: z.string().optional(),
  location: z.string().optional(),
});

const telemetrySchema = z.object({
  device_id: z.string().optional(),
  sensors: z.array(z.object({
    mac: z.string().min(3),
    name: z.string().optional(),
    brand: z.string().optional(),
    model: z.string().optional(),
    protocol: z.string().optional(),
    sensor_type: z.string().optional(),
    location: z.string().optional(),
    rssi: z.number().int().optional(),
    temperature_c: z.number().optional(),
    humidity_percent: z.number().optional(),
    battery_percent: z.number().int().optional(),
    raw_advertisement: z.string().optional(),
  })).default([]),
});

const updateSensorSchema = z.object({
  name: z.string().optional(),
  brand: z.string().optional(),
  model: z.string().optional(),
  protocol: z.string().optional(),
  sensor_type: z.string().optional(),
  location: z.string().optional(),
});

const singleTelemetrySchema = z.object({
  temperature_c: z.number().optional(),
  humidity_percent: z.number().optional(),
  battery_percent: z.number().int().optional(),
  rssi: z.number().int().optional(),
});

const childDeviceSchema = z.object({
  id: z.string().min(1).optional(),
  device_id: z.string().min(1).optional(),
  name: z.string().min(1),
  type: z.string().default("unknown"),
  brand: z.string().default("generic"),
  model: z.string().default("unknown"),
  protocol: z.string().default("unknown"),
  address: z.string().optional(),
  location: z.string().optional(),
  capabilities: z.array(z.string()).default([]),
  state: z.record(z.unknown()).optional(),
  raw: z.record(z.unknown()).optional(),
});

const childDevicePatchSchema = childDeviceSchema.partial();

const childStateSchema = z.object({
  state: z.record(z.unknown()),
  online: z.boolean().optional(),
  capabilities: z.array(z.string()).optional(),
});

const childControlSchema = z.object({
  capability: z.string().min(1),
  value: z.unknown(),
});

async function upsertCapabilities(deviceId: string, capabilities: string[]) {
  for (const capability of capabilities) {
    await prisma.deviceCapability.upsert({
      where: { deviceId_name: { deviceId, name: capability } },
      create: { deviceId, name: capability },
      update: {},
    });
  }
}

function toInputJson(value: Record<string, unknown> | undefined): Prisma.InputJsonObject | undefined {
  return value as Prisma.InputJsonObject | undefined;
}

async function ownedGateway(req: AuthRequest, gatewayDeviceId: string) {
  return prisma.device.findFirst({
    where: {
      deviceId: gatewayDeviceId,
      ownerId: req.userId,
    },
  });
}

gatewaysRouter.get("/:gateway_id/ble-sensors", requireUser, async (req: AuthRequest, res) => {
  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });

  const sensors = await prisma.bleSensor.findMany({
    where: { gatewayId: gateway.id },
    orderBy: [{ lastSeen: "desc" }, { updatedAt: "desc" }],
  });
  return res.json({ ok: true, sensors });
});

gatewaysRouter.get("/:gateway_id/devices", requireUser, async (req: AuthRequest, res) => {
  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });
  const devices = await prisma.childDevice.findMany({
    where: { gatewayId: gateway.id },
    include: { capabilities: true, states: { orderBy: { createdAt: "desc" }, take: 1 } },
    orderBy: [{ lastSeen: "desc" }, { updatedAt: "desc" }],
  });
  return res.json({ ok: true, devices });
});

gatewaysRouter.post("/:gateway_id/devices", requireUser, async (req: AuthRequest, res) => {
  const input = childDeviceSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid child device payload" });
  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });
  const externalId = input.data.device_id || input.data.id || input.data.address || input.data.name;
  const device = await prisma.childDevice.upsert({
    where: { gatewayId_externalId: { gatewayId: gateway.id, externalId } },
    create: {
      gatewayId: gateway.id,
      externalId,
      name: input.data.name,
      type: input.data.type,
      brand: input.data.brand,
      model: input.data.model,
      protocol: input.data.protocol,
      address: input.data.address,
      location: input.data.location,
      raw: toInputJson(input.data.raw),
      online: true,
      lastSeen: new Date(),
    },
    update: {
      name: input.data.name,
      type: input.data.type,
      brand: input.data.brand,
      model: input.data.model,
      protocol: input.data.protocol,
      address: input.data.address,
      location: input.data.location,
      raw: toInputJson(input.data.raw),
      online: true,
      lastSeen: new Date(),
    },
  });
  await upsertCapabilities(device.id, input.data.capabilities);
  if (input.data.state) {
    await prisma.deviceState.create({ data: { deviceId: device.id, state: toInputJson(input.data.state)! } });
  }
  return res.status(201).json({ ok: true, device });
});

gatewaysRouter.patch("/:gateway_id/devices/:device_id", requireUser, async (req: AuthRequest, res) => {
  const input = childDevicePatchSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid child device update payload" });
  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });
  const updated = await prisma.childDevice.updateMany({
    where: { gatewayId: gateway.id, externalId: String(req.params.device_id) },
    data: {
      name: input.data.name,
      type: input.data.type,
      brand: input.data.brand,
      model: input.data.model,
      protocol: input.data.protocol,
      address: input.data.address,
      location: input.data.location,
      raw: toInputJson(input.data.raw),
    },
  });
  if (updated.count === 0) return res.status(404).json({ ok: false, error: "Child device not found" });
  const device = await prisma.childDevice.findFirst({ where: { gatewayId: gateway.id, externalId: String(req.params.device_id) } });
  return res.json({ ok: true, device });
});

gatewaysRouter.delete("/:gateway_id/devices/:device_id", requireUser, async (req: AuthRequest, res) => {
  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });
  const deleted = await prisma.childDevice.deleteMany({ where: { gatewayId: gateway.id, externalId: String(req.params.device_id) } });
  if (deleted.count === 0) return res.status(404).json({ ok: false, error: "Child device not found" });
  return res.json({ ok: true });
});

gatewaysRouter.post("/:gateway_id/devices/:device_id/state", requireDevice, async (req: AuthRequest, res) => {
  if (!req.device || req.device.deviceId !== String(req.params.gateway_id)) {
    return res.status(403).json({ ok: false, error: "Device token does not match gateway" });
  }
  const input = childStateSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid child device state payload" });
  const child = await prisma.childDevice.findFirst({ where: { gatewayId: req.device.id, externalId: String(req.params.device_id) } });
  if (!child) return res.status(404).json({ ok: false, error: "Child device not found" });
  const state = await prisma.deviceState.create({ data: { deviceId: child.id, state: toInputJson(input.data.state)! } });
  await prisma.childDevice.update({ where: { id: child.id }, data: { online: input.data.online ?? true, lastSeen: state.createdAt } });
  if (input.data.capabilities) await upsertCapabilities(child.id, input.data.capabilities);
  return res.status(201).json({ ok: true, state });
});

gatewaysRouter.post("/:gateway_id/devices/:device_id/control", requireUser, async (req: AuthRequest, res) => {
  const input = childControlSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid control payload" });
  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });
  const child = await prisma.childDevice.findFirst({ where: { gatewayId: gateway.id, externalId: String(req.params.device_id) } });
  if (!child) return res.status(404).json({ ok: false, error: "Child device not found" });
  await prisma.deviceLog.create({ data: { deviceId: child.id, level: "info", message: `Control requested: ${input.data.capability}` } });
  return res.status(202).json({ ok: true, queued: true });
});

gatewaysRouter.get("/:gateway_id/devices/:device_id/logs", requireUser, async (req: AuthRequest, res) => {
  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });
  const child = await prisma.childDevice.findFirst({ where: { gatewayId: gateway.id, externalId: String(req.params.device_id) } });
  if (!child) return res.status(404).json({ ok: false, error: "Child device not found" });
  const logs = await prisma.deviceLog.findMany({ where: { deviceId: child.id }, orderBy: { createdAt: "desc" }, take: 100 });
  return res.json({ ok: true, logs });
});

gatewaysRouter.post("/:gateway_id/ble-sensors", requireUser, async (req: AuthRequest, res) => {
  const input = sensorSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid BLE sensor payload" });

  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });

  const sensor = await prisma.bleSensor.upsert({
    where: {
      gatewayId_macAddress: {
        gatewayId: gateway.id,
        macAddress: input.data.mac,
      },
    },
    create: {
      gatewayId: gateway.id,
      macAddress: input.data.mac,
      name: input.data.name,
      brand: input.data.brand || "unknown",
      model: input.data.model || "unknown",
      protocol: input.data.protocol || input.data.sensor_type || "unknown",
      sensorType: input.data.sensor_type || "unknown_ble_sensor",
      location: input.data.location,
    },
    update: {
      name: input.data.name,
      brand: input.data.brand || "unknown",
      model: input.data.model || "unknown",
      protocol: input.data.protocol || input.data.sensor_type || "unknown",
      sensorType: input.data.sensor_type || "unknown_ble_sensor",
      location: input.data.location,
    },
  });
  return res.status(201).json({ ok: true, sensor });
});

gatewaysRouter.patch("/:gateway_id/ble-sensors/:sensor_id", requireUser, async (req: AuthRequest, res) => {
  const input = updateSensorSchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid BLE sensor update payload" });

  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });

  const sensor = await prisma.bleSensor.updateMany({
    where: { id: String(req.params.sensor_id), gatewayId: gateway.id },
    data: {
      name: input.data.name,
      brand: input.data.brand,
      model: input.data.model,
      protocol: input.data.protocol,
      sensorType: input.data.sensor_type,
      location: input.data.location,
    },
  });
  if (sensor.count === 0) return res.status(404).json({ ok: false, error: "BLE sensor not found" });
  const updated = await prisma.bleSensor.findUnique({ where: { id: String(req.params.sensor_id) } });
  return res.json({ ok: true, sensor: updated });
});

gatewaysRouter.delete("/:gateway_id/ble-sensors/:sensor_id", requireUser, async (req: AuthRequest, res) => {
  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });

  const deleted = await prisma.bleSensor.deleteMany({
    where: { id: String(req.params.sensor_id), gatewayId: gateway.id },
  });
  if (deleted.count === 0) return res.status(404).json({ ok: false, error: "BLE sensor not found" });
  return res.json({ ok: true });
});

gatewaysRouter.post("/:gateway_id/ble-sensors/telemetry", requireDevice, async (req: AuthRequest, res) => {
  const gatewayDeviceId = String(req.params.gateway_id);
  if (!req.device || req.device.deviceId !== gatewayDeviceId) {
    return res.status(403).json({ ok: false, error: "Device token does not match gateway" });
  }

  const input = telemetrySchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid BLE telemetry payload" });

  const now = new Date();
  const results = [];
  for (const item of input.data.sensors) {
    const sensor = await prisma.bleSensor.upsert({
      where: {
        gatewayId_macAddress: {
          gatewayId: req.device.id,
          macAddress: item.mac,
        },
      },
      create: {
        gatewayId: req.device.id,
        macAddress: item.mac,
        name: item.name,
        brand: item.brand || "unknown",
        model: item.model || "unknown",
        protocol: item.protocol || item.sensor_type || "unknown",
        sensorType: item.sensor_type || "unknown_ble_sensor",
        location: item.location,
        rssi: item.rssi,
        temperatureC: item.temperature_c,
        humidityPercent: item.humidity_percent,
        batteryPercent: item.battery_percent,
        rawAdvertisement: item.raw_advertisement,
        lastSeen: now,
      },
      update: {
        name: item.name,
        brand: item.brand || "unknown",
        model: item.model || "unknown",
        protocol: item.protocol || item.sensor_type || "unknown",
        sensorType: item.sensor_type || "unknown_ble_sensor",
        location: item.location,
        rssi: item.rssi,
        temperatureC: item.temperature_c,
        humidityPercent: item.humidity_percent,
        batteryPercent: item.battery_percent,
        rawAdvertisement: item.raw_advertisement,
        lastSeen: now,
      },
    });
    await prisma.bleSensorTelemetry.create({
      data: {
        sensorId: sensor.id,
        temperatureC: item.temperature_c,
        humidityPercent: item.humidity_percent,
        batteryPercent: item.battery_percent,
        rssi: item.rssi,
      },
    });
    results.push(sensor);
  }

  await prisma.device.update({
    where: { id: req.device.id },
    data: { online: true, lastSeen: now },
  });

  return res.json({ ok: true, sensors: results.length });
});

gatewaysRouter.post("/:gateway_id/ble-sensors/:sensor_id/telemetry", requireDevice, async (req: AuthRequest, res) => {
  const gatewayDeviceId = String(req.params.gateway_id);
  if (!req.device || req.device.deviceId !== gatewayDeviceId) {
    return res.status(403).json({ ok: false, error: "Device token does not match gateway" });
  }
  const input = singleTelemetrySchema.safeParse(req.body);
  if (!input.success) return res.status(400).json({ ok: false, error: "Invalid BLE telemetry payload" });

  const sensor = await prisma.bleSensor.findFirst({
    where: { id: String(req.params.sensor_id), gatewayId: req.device.id },
  });
  if (!sensor) return res.status(404).json({ ok: false, error: "BLE sensor not found" });

  const telemetry = await prisma.bleSensorTelemetry.create({
    data: {
      sensorId: sensor.id,
      temperatureC: input.data.temperature_c,
      humidityPercent: input.data.humidity_percent,
      batteryPercent: input.data.battery_percent,
      rssi: input.data.rssi,
    },
  });
  await prisma.bleSensor.update({
    where: { id: sensor.id },
    data: {
      temperatureC: input.data.temperature_c,
      humidityPercent: input.data.humidity_percent,
      batteryPercent: input.data.battery_percent,
      rssi: input.data.rssi,
      lastSeen: telemetry.createdAt,
    },
  });
  return res.status(201).json({ ok: true, telemetry });
});

gatewaysRouter.get("/:gateway_id/ble-sensors/:sensor_id/telemetry/latest", requireUser, async (req: AuthRequest, res) => {
  const gateway = await ownedGateway(req, String(req.params.gateway_id));
  if (!gateway) return res.status(404).json({ ok: false, error: "Gateway not found" });

  const sensor = await prisma.bleSensor.findFirst({
    where: { id: String(req.params.sensor_id), gatewayId: gateway.id },
  });
  if (!sensor) return res.status(404).json({ ok: false, error: "BLE sensor not found" });

  const telemetry = await prisma.bleSensorTelemetry.findFirst({
    where: { sensorId: sensor.id },
    orderBy: { createdAt: "desc" },
  });
  return res.json({ ok: true, telemetry });
});
