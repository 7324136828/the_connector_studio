import { test, expect } from "@playwright/test";
import { createProject, selectProject } from "../helpers/project-sessions";

function deferred<T>() {
  let resolve!: (value: T | PromiseLike<T>) => void;
  const promise = new Promise<T>((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

test("a delayed session refresh cannot replace a newer saved title and draft", async ({ page }) => {
  const project = await createProject(page, "Session refresh project");
  await page.goto("/");
  await selectProject(page, project);
  const createdResponse = page.waitForResponse(
    (response) =>
      new URL(response.url()).pathname === "/api/sessions" &&
      response.request().method() === "POST",
  );
  await page.locator(".empty-workspace").getByRole("button", { name: "New Session", exact: true }).click();
  const session = (await (await createdResponse).json()) as { id: string };
  const title = page.getByLabel("Session title", { exact: true });
  const draft = page.getByLabel("Message", { exact: true });
  await expect(title).toHaveValue("New session");
  await expect(draft).toHaveValue("");

  const captured = deferred<Array<{ id: string; title: string; draft: string }>>();
  const release = deferred<void>();
  let held = false;
  await page.route("**/api/sessions?include_agents=true", async (route) => {
    if (route.request().method() !== "GET" || held) {
      await route.continue();
      return;
    }
    held = true;
    const stale = await route.fetch();
    captured.resolve(await stale.json());
    await release.promise;
    await route.fulfill({
      response: stale,
      headers: { ...stale.headers(), "x-regression-stale": "1" },
    });
  });

  try {
    // Capture the periodic list response before editing, then hold its delivery.
    const old = (await captured.promise).find((item) => item.id === session.id);
    expect(old).toMatchObject({ title: "New session", draft: "" });

    const latestTitle = "Saved refresh regression " + Date.now();
    const latestDraft = "This draft must survive the older refresh response.";
    const newerList = page.waitForResponse(async (response) => {
      if (
        new URL(response.url()).pathname !== "/api/sessions" ||
        response.request().method() !== "GET" ||
        !response.ok() ||
        response.headers()["x-regression-stale"]
      ) return false;
      const items = (await response.json()) as Array<{ id: string; title: string; draft: string }>;
      return items.some(
        (item) => item.id === session.id && item.title === latestTitle && item.draft === latestDraft,
      );
    });
    const saved = page.waitForResponse(
      (response) =>
        new URL(response.url()).pathname === `/api/sessions/${session.id}/save` &&
        response.request().method() === "POST",
    );
    await title.fill(latestTitle);
    await draft.fill(latestDraft);
    await page.keyboard.press(process.platform === "darwin" ? "Meta+s" : "Control+s");
    expect((await saved).ok()).toBeTruthy();
    await newerList;
    await expect(page.getByRole("status").filter({ hasText: "Session saved." })).toBeVisible();
    const stored = await page.request.get(`/api/sessions/${session.id}`);
    expect(await stored.json()).toMatchObject({ title: latestTitle, draft: latestDraft });

    const delivered = page.waitForResponse(
      (response) => response.headers()["x-regression-stale"] === "1",
    );
    release.resolve();
    await (await delivered).finished();
    // Wait for the delivered fetch and React state to commit on a browser paint.
    await page.evaluate(() => new Promise<void>((resolve) => {
      requestAnimationFrame(() => requestAnimationFrame(() => resolve()));
    }));
    await expect(title).toHaveValue(latestTitle);
    await expect(draft).toHaveValue(latestDraft);
  } finally {
    release.resolve();
    await page.unroute("**/api/sessions?include_agents=true");
  }
});
