import { useRef, useState } from "react";
import { type Project } from "../services/api";
import FileBrowser, { useDialogFocus } from "./FileBrowser";
import "./project-dialog.css";

type Category = "files" | "sessions" | "projects";
export interface NewItemDialogProps {
  category: Category;
  projects: Project[];
  initialParentPath: string;
  currentProject: Project | null;
  busy: boolean;
  onClose: () => void;
  onCreate: (
    kind: "file" | "session" | "project",
    name: string,
    parentPath: string,
  ) => Promise<void>;
  onOpenProject: (path: string) => Promise<void>;
}

function childPath(parent: string, child: string) {
  const separator = parent.includes("\\") ? "\\" : "/";
  return parent.replace(/[\\/]+$/, "") + separator + child;
}

export default function NewItemDialog({
  category: initialCategory,
  projects,
  initialParentPath,
  currentProject,
  busy,
  onClose,
  onCreate,
  onOpenProject,
}: NewItemDialogProps) {
  const [category, setCategory] = useState<Category>(initialCategory);
  const [name, setName] = useState("");
  const [parentPath, setParentPath] = useState(initialParentPath);
  const [selectedProject, setSelectedProject] = useState<string | null>(null);
  const [browser, setBrowser] = useState<"parent" | "open" | null>(null);
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState("");
  const dialogRef = useRef<HTMLDivElement>(null);
  const working = busy || submitting;
  useDialogFocus(
    dialogRef,
    () => {
      if (!working) onClose();
    },
    browser === null,
  );
  const projectReady = Boolean(
    category === "sessions" ? currentProject : currentProject?.managed,
  );
  const destination =
    category === "projects"
      ? parentPath
      : currentProject
        ? childPath(currentProject.path, category)
        : "";
  const canCreate =
    name.trim().length > 0 &&
    Boolean(destination.trim()) &&
    (category === "projects" || projectReady);
  const item =
    category === "projects"
      ? {
          title: "Work Project",
          subtitle: "Project folder",
          description:
            "Create a project folder containing files and saved work sessions.",
          help: "Creates a project folder for your files and saved work sessions.",
        }
      : category === "sessions"
        ? {
            title: "Work Session",
            subtitle: "Saved work session",
            description: "Create a new work session in the current project.",
            help: "Creates a saved work session for the current project.",
          }
        : {
            title: "Text File",
            subtitle: "Project file",
            description:
              "Create an empty text file in the current project's files folder.",
            help: "Creates a file in the current project. Include a file extension, such as notes.txt.",
          };

  const changeCategory = (next: Category) => {
    if (working) return;
    setCategory(next);
    setName("");
    setError("");
  };

  const create = async (event: React.FormEvent) => {
    event.preventDefault();
    if (!canCreate || working) return;
    setSubmitting(true);
    setError("");
    try {
      await onCreate(
        category === "projects"
          ? "project"
          : category === "sessions"
            ? "session"
            : "file",
        name.trim(),
        destination.trim(),
      );
    } catch (reason) {
      setError(
        reason instanceof Error
          ? reason.message
          : "Unable to create this item. Please try again.",
      );
    } finally {
      setSubmitting(false);
    }
  };

  const openProject = async (path: string) => {
    if (working) return;
    setSubmitting(true);
    setError("");
    try {
      await onOpenProject(path);
    } catch (reason) {
      setError(
        reason instanceof Error
          ? reason.message
          : "Unable to open this project. Please try again.",
      );
    } finally {
      setSubmitting(false);
    }
  };

  const chooseFolder = (path: string) => {
    const mode = browser;
    setBrowser(null);
    if (mode === "parent") {
      setParentPath(path);
      setError("");
    } else if (mode === "open") {
      void openProject(path);
    }
  };

  return (
    <>
      <div
        className="project-dialog-overlay"
        onMouseDown={(event) => {
          if (event.target === event.currentTarget && !working && !browser)
            onClose();
        }}
      >
        <div
          className="new-item-dialog"
          data-testid="new-item-dialog"
          role="dialog"
          aria-modal={browser ? undefined : true}
          aria-labelledby="new-item-title"
          aria-hidden={browser ? true : undefined}
          ref={dialogRef}
          tabIndex={-1}
        >
          <header className="project-dialog-heading">
            <h2 id="new-item-title">New</h2>
            <button
              className="project-dialog-close"
              aria-label="Close new item dialog"
              onClick={onClose}
              disabled={working}
            >
              ×
            </button>
          </header>
          <form onSubmit={create} className="new-item-form">
            <div
              className="new-item-tabs"
              role="tablist"
              aria-label="New item categories"
            >
              {(["files", "sessions", "projects"] as Category[]).map((tab) => (
                <button
                  type="button"
                  role="tab"
                  aria-selected={category === tab}
                  aria-controls="new-item-panel"
                  id={`new-item-tab-${tab}`}
                  key={tab}
                  className={category === tab ? "selected" : ""}
                  onClick={() => changeCategory(tab)}
                  disabled={working}
                  onKeyDown={(event) => {
                    const tabs: Category[] = ["files", "sessions", "projects"];
                    const index = tabs.indexOf(tab);
                    const next =
                      event.key === "ArrowRight"
                        ? tabs[(index + 1) % tabs.length]
                        : event.key === "ArrowLeft"
                          ? tabs[(index + tabs.length - 1) % tabs.length]
                          : event.key === "Home"
                            ? tabs[0]
                            : event.key === "End"
                              ? tabs[tabs.length - 1]
                              : null;
                    if (next) {
                      event.preventDefault();
                      changeCategory(next);
                      document.getElementById(`new-item-tab-${next}`)?.focus();
                    }
                  }}
                >
                  {tab.charAt(0).toUpperCase() + tab.slice(1)}
                </button>
              ))}
            </div>
            <section
              className="new-item-content"
              role="tabpanel"
              id="new-item-panel"
              aria-labelledby={`new-item-tab-${category}`}
            >
              <div className="new-item-fields">
                <div className="new-item-types">
                  <p className="project-dialog-label">Item type</p>
                  <div
                    className="new-item-type-list"
                    role="listbox"
                    aria-label="Item type"
                  >
                    <div
                      className="new-item-type selected"
                      role="option"
                      aria-selected="true"
                    >
                      <strong>{item.title}</strong>
                      <span>{item.subtitle}</span>
                    </div>
                  </div>
                </div>
                <div className="new-item-details">
                  <label
                    className="project-dialog-label"
                    htmlFor="new-item-name"
                  >
                    Name
                  </label>
                  <input
                    id="new-item-name"
                    aria-label={
                      category === "projects"
                        ? "Project name"
                        : category === "sessions"
                          ? "Session name"
                          : "File name"
                    }
                    data-dialog-autofocus
                    value={name}
                    onChange={(event) => setName(event.target.value)}
                    maxLength={100}
                    disabled={
                      working || (category !== "projects" && !projectReady)
                    }
                    autoComplete="off"
                  />
                  <label
                    className="project-dialog-label new-item-parent-label"
                    htmlFor="new-item-parent"
                  >
                    Parent folder
                  </label>
                  <div className="new-item-parent-row">
                    <input
                      id="new-item-parent"
                      aria-label="Parent folder"
                      value={destination}
                      onChange={(event) => setParentPath(event.target.value)}
                      readOnly={category !== "projects"}
                      disabled={working}
                      autoComplete="off"
                      title={destination}
                    />
                    {category === "projects" && (
                      <button
                        type="button"
                        className="project-dialog-button"
                        onClick={() => setBrowser("parent")}
                        disabled={working}
                      >
                        Browse...
                      </button>
                    )}
                  </div>
                  <p className="new-item-description">{item.description}</p>
                  {category !== "projects" && !projectReady && (
                    <p className="new-item-project-needed">
                      {currentProject
                        ? "Create or open a Work Project to create files in its files folder."
                        : "Create or open a Work Project to save files and sessions."}{" "}
                      <button
                        type="button"
                        onClick={() => changeCategory("projects")}
                      >
                        Go to Projects
                      </button>
                    </p>
                  )}
                </div>
              </div>
              <p className="new-item-help">{item.help}</p>
              {category === "projects" && (
                <div className="new-item-recent">
                  <p className="project-dialog-label">Recent projects</p>
                  <div
                    className="new-item-recent-list"
                    role="listbox"
                    aria-label="Recent projects"
                  >
                    {projects.length === 0 ? (
                      <p className="new-item-empty">
                        Your recent projects will appear here.
                      </p>
                    ) : (
                      projects.slice(0, 5).map((project) => (
                        <button
                          type="button"
                          role="option"
                          aria-selected={selectedProject === project.id}
                          className={`new-item-recent-project${selectedProject === project.id ? " selected" : ""}`}
                          key={project.id}
                          onClick={() => setSelectedProject(project.id)}
                          onDoubleClick={() => void openProject(project.path)}
                          disabled={working}
                        >
                          <strong>{project.name}</strong>
                          <span>{project.path}</span>
                        </button>
                      ))
                    )}
                  </div>
                  <div className="new-item-open-actions">
                    <button
                      type="button"
                      className="project-dialog-button"
                      disabled={working || !selectedProject}
                      onClick={() => {
                        const project = projects.find(
                          (entry) => entry.id === selectedProject,
                        );
                        if (project) void openProject(project.path);
                      }}
                    >
                      Open Selected
                    </button>
                    <button
                      type="button"
                      className="project-dialog-button"
                      disabled={working}
                      onClick={() => setBrowser("open")}
                    >
                      Open Existing...
                    </button>
                  </div>
                </div>
              )}
              {error && (
                <p className="project-dialog-error" role="alert">
                  {error}
                </p>
              )}
            </section>
            <footer className="new-item-footer project-dialog-actions">
              <button
                type="button"
                className="project-dialog-button"
                onClick={onClose}
                disabled={working}
              >
                Cancel
              </button>
              <button
                className="project-dialog-button primary"
                disabled={working || !canCreate}
              >
                {working ? "Working…" : "Create"}
              </button>
            </footer>
          </form>
        </div>
      </div>
      {browser && (
        <FileBrowser
          initialPath={parentPath || initialParentPath}
          onSelect={chooseFolder}
          onClose={() => setBrowser(null)}
        />
      )}
    </>
  );
}
