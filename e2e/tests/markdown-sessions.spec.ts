import { test, expect, type Page } from "@playwright/test";
import { copyFileSync, existsSync, readFileSync } from "node:fs";
import { basename, dirname, join } from "node:path";
import { createProject, selectProject, openProjectSession, readNativeSnapshot } from "../helpers/project-sessions";

async function readSessionFile(page: Page, path: string) {
  return readNativeSnapshot(page, readFileSync(path));
}

test("Markdown renders multiline and flattened tables without altering code or ordinary pipes", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1280, height: 1000 });
  const project = await createProject(page, "Markdown conversation");
  await page.goto("/");
  await selectProject(page, project);
  await page.getByRole("button", { name: "Add session", exact: true }).click();
  await page.getByLabel("Model", { exact: true }).selectOption("test-config");
  await page.getByLabel("Message", { exact: true }).fill("markdown tables");
  await page.getByRole("button", { name: /Send$/ }).click();
  const reply = page.locator(".message.assistant");
  await expect(reply.getByRole("table")).toHaveCount(3, { timeout: 15000 });
  for (const index of [0, 1]) {
    const table = reply.getByRole("table").nth(index);
    await expect(table.getByRole("columnheader")).toHaveText([
      "Feature",
      "APC",
      "DPC",
    ]);
    await expect(table.getByRole("row")).toHaveCount(6);
    await expect(
      table.getByRole("cell").filter({ hasText: "Specific thread" }),
    ).toBeVisible();
    await expect(table.locator("strong").first()).toHaveText("Target");
  }
  await expect(
    reply.getByRole("table").nth(2).getByRole("cell").last(),
  ).toHaveText("a | b");
  await expect(reply.locator("pre code")).toHaveText(
    "| Code | Text | | --- | --- | | a | b |\n",
  );
  await expect(reply.locator("p code")).toHaveText(
    "| Inline | Text | | --- | --- | | a | b |",
  );
  await expect(reply).toContainText("Pipes are ordinary prose: a | b | c.");
  await reply.getByRole("table").first().scrollIntoViewIfNeeded();
  await page.screenshot({
    path: "test-results/markdown-tables.png",
    fullPage: true,
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await reply.getByRole("table").first().scrollIntoViewIfNeeded();
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= window.innerWidth,
    ),
  ).toBeTruthy();
  const region = reply
    .getByRole("region", { name: "Message table", exact: true })
    .first();
  expect(
    await region.evaluate(
      (element) => element.scrollWidth >= element.clientWidth,
    ),
  ).toBeTruthy();
  await page.screenshot({
    path: "test-results/markdown-tables-mobile.png",
    fullPage: true,
  });
});

test("session files open conversations, autosave replies and support Ctrl+S outside the composer", async ({
  page,
}) => {
  const project = await (
    await page.request.post("/api/projects", {
      data: { name: "File sessions " + Date.now() },
    })
  ).json();
  const original = await (
    await page.request.post("/api/sessions", {
      data: { title: "File conversation", project_id: project.id },
    })
  ).json();
  const job = await (
    await page.request.post("/api/sessions/" + original.id + "/messages", {
      data: { text: "Initial saved exchange", model: "test-config" },
    })
  ).json();
  await expect
    .poll(
      async () =>
        (await (await page.request.get("/api/jobs/" + job.id)).json()).status,
    )
    .toBe("completed");
  await page.request.patch("/api/sessions/" + original.id, {
    data: { draft: "Initial file draft" },
  });
  const saved = await (
    await page.request.post("/api/sessions/" + original.id + "/save")
  ).json();
  const filePath = join(project.path, "files", "review.lattice");
  copyFileSync(saved.saved_path, filePath);
  const sourceBytes = readFileSync(filePath);
  await page.request.post("/api/projects/" + project.id + "/files", {
    data: { name: "notes.md", content: "Ordinary file" },
  });
  const other = await (
    await page.request.post("/api/sessions", {
      data: { title: "Other conversation", project_id: project.id },
    })
  ).json();
  await page.request.patch("/api/sessions/" + other.id, {
    data: { draft: "Keep other draft" },
  });
  await page.goto("/");
  await selectProject(page, project);
  await openProjectSession(page, project, other);
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue(
    "Keep other draft",
  );
  await page
    .locator(".file-list")
    .getByTitle("files/notes.md", { exact: true })
    .click();
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue(
    /@files\/notes\.md/,
  );
  const openResponse = page.waitForResponse(
    (response) =>
      response
        .url()
        .endsWith("/api/projects/" + project.id + "/sessions/open") &&
      response.request().method() === "POST",
  );
  const sessionEntry = page
    .locator(".file-list")
    .getByTitle("files/review.lattice", { exact: true });
  await sessionEntry.click();
  const opened = await (await openResponse).json();
  let canonicalPath: string = opened.saved_path;
  expect(dirname(canonicalPath)).toBe(join(project.path, "sessions"));
  expect(basename(canonicalPath)).toBe("File conversation (2).lattice");
  expect(readFileSync(filePath)).toEqual(sourceBytes);
  expect(opened.id).not.toBe(original.id);
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(
    "File conversation",
  );
  await expect(page.locator(".message.assistant")).toContainText(
    "Initial saved exchange",
  );
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue(
    "Initial file draft",
  );
  await expect(page.locator(".attachments")).toHaveCount(0);
  await page
    .getByLabel("Session title", { exact: true })
    .fill("Renamed file conversation");
  const shortcutSave = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions/" + opened.id + "/save") &&
      response.request().method() === "POST",
  );
  await page.getByLabel("Session title", { exact: true }).press("Control+s");
  const shortcutResponse = await shortcutSave;
  expect(shortcutResponse.status()).toBe(200);
  const renamedSaved = await shortcutResponse.json();
  const previousPath = canonicalPath;
  canonicalPath = renamedSaved.saved_path;
  expect(dirname(canonicalPath)).toBe(join(project.path, "sessions"));
  expect(basename(canonicalPath)).toBe("Renamed file conversation.lattice");
  expect(existsSync(previousPath)).toBe(false);
  const renamed = await readSessionFile(page, canonicalPath);
  expect(renamed.title).toBe("Renamed file conversation");
  expect(renamed.draft).toBe("Initial file draft");
  await expect(
    page.getByRole("status").filter({ hasText: "Session saved." }),
  ).toBeVisible();
  await page
    .getByLabel("Message", { exact: true })
    .fill("New autosaved exchange");
  await page.getByRole("button", { name: /Send$/ }).click();
  await expect(page.locator(".message.assistant").last()).toContainText(
    "New autosaved exchange",
    { timeout: 15000 },
  );
  const autosaved = await readSessionFile(page, canonicalPath);
  expect(autosaved.messages).toHaveLength(4);
  expect(autosaved.messages[3].text).toContain("New autosaved exchange");
  expect(autosaved.title).toBe("Renamed file conversation");
  await page
    .getByLabel("Message", { exact: true })
    .fill("Saved with focus outside composer");
  await page.locator(".message.assistant .message-meta strong").last().click();
  const outsideSave = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions/" + opened.id + "/save") &&
      response.request().method() === "POST",
  );
  await page.keyboard.press("Control+s");
  expect((await outsideSave).status()).toBe(200);
  expect((await readSessionFile(page, canonicalPath)).draft).toBe(
    "Saved with focus outside composer",
  );
  const reopenResponse = page.waitForResponse(
    (response) =>
      response
        .url()
        .endsWith("/api/projects/" + project.id + "/sessions/open") &&
      response.request().method() === "POST",
  );
  await sessionEntry.click();
  expect((await (await reopenResponse).json()).id).toBe(opened.id);
  await expect(
    page.getByRole("tab").filter({ hasText: "Renamed file conversation" }),
  ).toHaveCount(1);
  const keptOther = await (
    await page.request.get("/api/sessions/" + other.id)
  ).json();
  expect(keptOther.draft).toContain("Keep other draft @files/notes.md");
  expect(keptOther.draft).not.toContain("review.lattice");
  expect(readFileSync(filePath)).toEqual(sourceBytes);
  await page
    .getByLabel("Message", { exact: true })
    .fill("Pending draft in file tab");
  await page.getByRole("tab").filter({ hasText: "Other conversation" }).click();
  await sessionEntry.click();
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue(
    "Pending draft in file tab",
  );
  const finalSave = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions/" + opened.id + "/save") &&
      response.request().method() === "POST",
  );
  await page.keyboard.press("Control+s");
  expect((await finalSave).status()).toBe(200);
  await page.reload();
  await selectProject(page, project);
  await page
    .locator(".file-list")
    .getByTitle("files/review.lattice", { exact: true })
    .click();
  await expect(page.getByLabel("Session title", { exact: true })).toHaveValue(
    "Renamed file conversation",
  );
  await expect(page.locator(".message.assistant")).toHaveCount(2);
  await expect(page.getByLabel("Message", { exact: true })).toHaveValue(
    "Pending draft in file tab",
  );
});

test("slow draft writes serialize blur and Ctrl+S without losing the latest edit", async ({
  page,
}) => {
  const project = await createProject(page, "Markdown conversation");
  await page.goto("/");
  await selectProject(page, project);
  await page.getByRole("button", { name: "Add session", exact: true }).click();
  const draft = page.getByLabel("Message", { exact: true });
  const title = page.getByLabel("Session title", { exact: true });
  let releaseFirst = () => {};
  let releaseNext = () => {};
  const firstGate = new Promise<void>((resolve) => {
    releaseFirst = resolve;
  });
  const nextGate = new Promise<void>((resolve) => {
    releaseNext = resolve;
  });
  const patches: string[] = [];
  let activeRequests = 0;
  let maxActiveRequests = 0;
  await page.route(/\/api\/sessions\/[^/]+$/, async (route) => {
    const request = route.request();
    if (
      request.method() !== "PATCH" ||
      request.postDataJSON()?.draft === undefined
    ) {
      await route.continue();
      return;
    }
    const index = patches.length;
    patches.push(request.postDataJSON().draft);
    activeRequests++;
    maxActiveRequests = Math.max(maxActiveRequests, activeRequests);
    const response = await route.fetch();
    if (index === 0) await firstGate;
    if (index === 1) await nextGate;
    activeRequests--;
    await route.fulfill({ response });
  });
  await draft.fill("First draft");
  await expect.poll(() => patches.slice()).toContain("First draft");
  await draft.fill("Next draft");
  await title.click();
  await draft.click();
  await title.click();
  await draft.click();
  releaseFirst();
  await expect.poll(() => patches.length).toBeGreaterThanOrEqual(2);
  await draft.fill("Latest draft");
  const savedResponse = page.waitForResponse(
    (response) =>
      /\/api\/sessions\/[^/]+\/save$/.test(response.url()) &&
      response.request().method() === "POST",
  );
  await page.keyboard.press("Control+s");
  releaseNext();
  const response = await savedResponse;
  expect(response.status()).toBe(200);
  const saved = await response.json();
  expect(maxActiveRequests).toBe(1);
  expect(patches).toEqual(["First draft", "Next draft", "Latest draft"]);
  expect((await readSessionFile(page, saved.saved_path)).draft).toBe(
    "Latest draft",
  );
  await expect(draft).toHaveValue("Latest draft");
  await title.fill("Saved by clicking after title edit");
  const buttonSave = page.waitForResponse(
    (response) =>
      response.url().endsWith("/api/sessions/" + saved.id + "/save") &&
      response.request().method() === "POST",
  );
  await page.getByRole("button", { name: "Save", exact: true }).click();
  const buttonResponse = await buttonSave;
  expect(buttonResponse.status()).toBe(200);
  const renamedSaved = await buttonResponse.json();
  expect(basename(renamedSaved.saved_path)).toBe(
    "Saved by clicking after title edit.lattice",
  );
  expect(existsSync(saved.saved_path)).toBe(false);
  expect((await readSessionFile(page, renamedSaved.saved_path)).title).toBe(
    "Saved by clicking after title edit",
  );
});
