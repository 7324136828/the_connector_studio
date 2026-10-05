import { test, expect, type Page } from "@playwright/test";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";

type Project = { id: string; name: string; path: string };
type Session = { id: string; model: string; project_id: string | null };
type Interaction = {
  job_id: string;
  session_id: string;
  model: string;
  user_text: string;
  assistant_text: string;
};
type ProjectMemory = { interaction_count: number; interactions: Interaction[] };

async function createProject(page: Page, label: string): Promise<Project> {
  const response = await page.request.post("/api/projects", {
    data: { name: label + " " + Date.now() },
  });
  expect(response.status()).toBe(201);
  return response.json();
}

async function selectProject(page: Page, project: Project) {
  await page.getByRole("button", { name: "New Project", exact: true }).click();
  const dialog = page.getByTestId("new-item-dialog");
  await dialog.getByRole("option").filter({ hasText: project.name }).click();
  await dialog
    .getByRole("button", { name: "Open Selected", exact: true })
    .click();
  await expect(dialog).not.toBeVisible();
  await expect(page.locator(".project-name")).toContainText(project.name);
}

async function addSession(page: Page): Promise<Session> {
  const createdResponse = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions") &&
      response.request().method() === "POST",
  );
  await page.getByRole("button", { name: "Add session", exact: true }).click();
  const response = await createdResponse;
  expect(response.status()).toBe(201);
  return response.json();
}

test("each project remembers its selected configuration for new sessions and after reload", async ({
  page,
}) => {
  const project = await createProject(page, "Remember configuration");
  await page.goto("/");
  await selectProject(page, project);
  await addSession(page);
  await page
    .getByLabel("Model", { exact: true })
    .selectOption("test-config-alt");
  await expect(page.locator(".project-memory-controls")).toContainText(
    "Default model: Alternate test configuration",
  );
  await expect
    .poll(
      async () =>
        (
          await (
            await page.request.get(
              "/api/projects/" + project.id + "/preferences",
            )
          ).json()
        ).default_model,
    )
    .toBe("test-config-alt");

  const next = await addSession(page);
  expect(next.model).toBe("test-config-alt");
  await expect(page.getByLabel("Model", { exact: true })).toHaveValue(
    "test-config-alt",
  );

  await page.reload();
  await selectProject(page, project);
  const reopened = await addSession(page);
  expect(reopened.model).toBe("test-config-alt");
  await expect(page.getByLabel("Model", { exact: true })).toHaveValue(
    "test-config-alt",
  );
  const preference = JSON.parse(
    readFileSync(join(project.path, ".memory", "preferences.json"), "utf-8"),
  );
  expect(preference.default_model).toBe("test-config-alt");

  const independent = await createProject(page, "Independent configuration");
  await page.reload();
  await selectProject(page, independent);
  const unrelated = await addSession(page);
  expect(unrelated.model).toBe("");
  await expect(page.getByLabel("Model", { exact: true })).toHaveValue("");
  await expect(page.locator(".project-memory-controls")).not.toContainText(
    "Default model: Alternate test configuration",
  );
});

test("Project Memory shows completed exchanges from its own disk folder and stays scoped", async ({
  page,
}) => {
  const project = await createProject(page, "Remember completed work");
  await page.goto("/");
  await selectProject(page, project);
  const session = await addSession(page);
  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  const marker = "Remember the cobalt approval for this project";
  await page.getByLabel("Message", { exact: true }).fill(marker);
  await page.getByRole("button", { name: /Send$/ }).click();
  await expect(page.locator(".message.assistant")).toContainText(marker, {
    timeout: 15000,
  });
  await expect
    .poll(
      async () =>
        (
          await (
            await page.request.get("/api/projects/" + project.id + "/memory")
          ).json()
        ).interaction_count,
    )
    .toBe(1);
  const summary: ProjectMemory = await (
    await page.request.get("/api/projects/" + project.id + "/memory")
  ).json();
  const interaction = summary.interactions[0];
  expect(interaction.session_id).toBe(session.id);
  const recordPath = join(
    project.path,
    ".memory",
    "interactions",
    interaction.job_id + ".json",
  );
  expect(existsSync(recordPath)).toBeTruthy();
  const record = JSON.parse(readFileSync(recordPath, "utf-8"));
  expect(record.user_text).toBe(marker);
  expect(record.assistant_text).toContain(marker);

  await page
    .getByRole("button", { name: "Project Memory", exact: true })
    .click();
  const dialog = page.getByRole("dialog", {
    name: "Project Memory",
    exact: true,
  });
  await expect(dialog).toContainText(project.name);
  await expect(dialog.getByTestId("memory-interaction")).toHaveCount(1);
  await expect(dialog.getByTestId("memory-interaction")).toContainText(marker);
  await page.screenshot({
    path: "test-results/project-memory.png",
    fullPage: true,
  });
  await dialog.getByRole("button", { name: "Close", exact: true }).click();

  const independent = await createProject(page, "Separate memory");
  await page.reload();
  await selectProject(page, independent);
  await page
    .getByRole("button", { name: "Project Memory", exact: true })
    .click();
  await expect(dialog).toContainText(independent.name);
  await expect(dialog.getByTestId("memory-interaction")).toHaveCount(0);
  await expect(dialog).not.toContainText(marker);
});

test("an unavailable remembered configuration remains visible until the user chooses an available one", async ({
  page,
}) => {
  const project = await createProject(page, "Retired configuration");
  const response = await page.request.post("/api/sessions", {
    data: { project_id: project.id },
  });
  expect(response.status()).toBe(201);
  const original: Session = await response.json();
  expect(
    (
      await page.request.patch("/api/sessions/" + original.id, {
        data: { model: "test-config-alt" },
      })
    ).status(),
  ).toBe(200);
  await page.route("**/api/models", (route) =>
    route.fulfill({
      json: [{ id: "test-config", name: "Test configuration" }],
    }),
  );
  await page.goto("/");
  await selectProject(page, project);
  await addSession(page);
  const model = page.getByLabel("Model", { exact: true });
  await expect(model).toHaveValue("test-config-alt");
  await expect(model.locator('option[value="test-config-alt"]')).toContainText(
    "unavailable",
  );
  await page
    .getByLabel("Message", { exact: true })
    .fill("Use a currently available model");
  await expect(page.getByRole("button", { name: /Send$/ })).toBeDisabled();
  await model.selectOption("test-config");
  await expect(page.getByRole("button", { name: /Send$/ })).toBeEnabled();
  await expect
    .poll(
      async () =>
        (
          await (
            await page.request.get(
              "/api/projects/" + project.id + "/preferences",
            )
          ).json()
        ).default_model,
    )
    .toBe("test-config");
});
