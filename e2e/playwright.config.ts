import { defineConfig, devices } from "@playwright/test";
import { existsSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { createServer } from "node:net";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const isWindows = process.platform === "win32";
const pythonPath = join(
  projectRoot,
  ".venv",
  isWindows ? "Scripts/python.exe" : "bin/python",
);
const python = existsSync(pythonPath)
  ? pythonPath
  : isWindows
    ? "python"
    : "python3";
async function allocatePorts(): Promise<number[]> {
  const probes = await Promise.all(
    [0, 1, 2].map(async () => {
      const server = createServer();
      await new Promise<void>((resolve) =>
        server.listen(0, "127.0.0.1", resolve),
      );
      const address = server.address();
      if (!address || typeof address === "string")
        throw new Error("Cannot allocate test port");
      return { server, port: address.port };
    }),
  );
  const ports = probes.map((p) => p.port);
  await Promise.all(
    probes.map(
      (p) => new Promise<void>((resolve) => p.server.close(() => resolve())),
    ),
  );
  return ports;
}
// Playwright evaluates configuration again inside workers: inherit the same ports.
const ports = process.env.STUDIO_E2E_PORTS
  ? (JSON.parse(process.env.STUDIO_E2E_PORTS) as number[])
  : await allocatePorts();
process.env.STUDIO_E2E_PORTS = JSON.stringify(ports);
const [connectorPort, backendPort, frontendPort] = ports;
const backendUrl = "http://127.0.0.1:" + backendPort;
const frontendUrl = "http://127.0.0.1:" + frontendPort;
const connectorUrl = "http://127.0.0.1:" + connectorPort;
const testData =
  process.env.STUDIO_E2E_DATA ??
  mkdtempSync(join(tmpdir(), "connector-studio-e2e-"));
process.env.STUDIO_E2E_DATA = testData;
const environment = Object.fromEntries(
  Object.entries(process.env).filter(
    (entry): entry is [string, string] => entry[1] !== undefined,
  ),
);

export default defineConfig({
  testDir: "./tests",
  fullyParallel: false,
  workers: 1,
  forbidOnly: Boolean(process.env.CI),
  retries: process.env.CI ? 1 : 0,
  reporter: "html",
  use: { baseURL: frontendUrl, trace: "on-first-retry" },
  projects:
    process.platform === "darwin"
      ? [{ name: "webkit", use: { ...devices["Desktop Safari"] } }]
      : [
          {
            name: "edge",
            use: {
              ...devices["Desktop Edge"],
              channel: process.env.CI ? undefined : "msedge",
            },
          },
        ],
  webServer: [
    {
      command: `${JSON.stringify(python)} e2e/mock_connector.py --port ${connectorPort}`,
      cwd: projectRoot,
      url: connectorUrl + "/v1/models",
      reuseExistingServer: false,
    },
    {
      command: `${JSON.stringify(python)} -m uvicorn backend.app.main:app --host 127.0.0.1 --port ${backendPort}`,
      cwd: projectRoot,
      env: {
        ...environment,
        STUDIO_DATA_DIR: testData,
        WORKSPACE_ROOT: join(testData, "workspace"),
        FILE_BROWSER_ROOTS: "*",
        CONNECTOR_URL: connectorUrl,
        CORS_ORIGINS: frontendUrl,
        STUDIO_SERVE_FRONTEND: "0",
      },
      url: backendUrl + "/api/health",
      reuseExistingServer: false,
    },
    {
      command: `${isWindows ? "npm.cmd" : "npm"} run dev -- --host 127.0.0.1 --port ${frontendPort} --strictPort`,
      cwd: join(projectRoot, "frontend"),
      env: { ...environment, VITE_BACKEND_URL: backendUrl },
      url: frontendUrl,
      reuseExistingServer: false,
    },
  ],
});
