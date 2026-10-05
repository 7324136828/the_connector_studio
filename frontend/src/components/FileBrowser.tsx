import { useEffect, useRef, useState, type RefObject } from "react";
import { api } from "../services/api";
import "./project-dialog.css";

interface Location {
  name: string;
  path: string;
}
interface FilesystemRoots {
  default_path: string;
  roots: Location[];
  shortcuts: Location[];
}
interface FilesystemEntry {
  name: string;
  path: string;
  kind: "directory" | "file";
  size: number | null;
  modified_at: string | null;
}
interface DirectoryListing {
  path: string;
  parent_path: string | null;
  entries: FilesystemEntry[];
  total: number;
  offset: number;
  limit: number;
}
export interface FileBrowserProps {
  initialPath: string;
  onSelect: (path: string) => Promise<void> | void;
  onClose: () => void;
}

export function useDialogFocus(
  ref: RefObject<HTMLElement>,
  onClose: () => void,
  enabled = true,
) {
  const closeRef = useRef(onClose);
  closeRef.current = onClose;
  useEffect(() => {
    const previous =
      document.activeElement instanceof HTMLElement
        ? document.activeElement
        : null;
    return () => previous?.focus();
  }, []);
  useEffect(() => {
    if (!enabled) return;
    const dialog = ref.current;
    if (!dialog) return;
    const focusable = () =>
      Array.from(
        dialog.querySelectorAll<HTMLElement>(
          'button:not(:disabled), input:not(:disabled), [tabindex="0"]',
        ),
      ).filter((element) => element.getClientRects().length > 0);
    const initial = dialog.querySelector<HTMLElement>(
      "[data-dialog-autofocus]",
    );
    if (!dialog.contains(document.activeElement)) {
      (initial || focusable()[0] || dialog).focus();
    }
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        closeRef.current();
      } else if (event.key === "Tab") {
        const elements = focusable();
        const first = elements[0];
        const last = elements[elements.length - 1];
        if (!first) {
          event.preventDefault();
          dialog.focus();
        } else if (
          event.shiftKey &&
          (document.activeElement === first ||
            !dialog.contains(document.activeElement))
        ) {
          event.preventDefault();
          last.focus();
        } else if (
          !event.shiftKey &&
          (document.activeElement === last ||
            !dialog.contains(document.activeElement))
        ) {
          event.preventDefault();
          first.focus();
        }
      }
    };
    document.addEventListener("keydown", onKeyDown);
    return () => document.removeEventListener("keydown", onKeyDown);
  }, [enabled, ref]);
}

function errorMessage(error: unknown) {
  return error instanceof Error
    ? error.message
    : "Unable to read this folder. Please try again.";
}

function FolderIcon({ small = false }: { small?: boolean }) {
  return (
    <svg
      className={small ? "fs-folder-icon fs-folder-small" : "fs-folder-icon"}
      viewBox="0 0 80 64"
      aria-hidden="true"
    >
      <path
        d="M4 10a5 5 0 0 1 5-5h22l8 8h32a5 5 0 0 1 5 5v37a5 5 0 0 1-5 5H9a5 5 0 0 1-5-5Z"
        fill="#c79528"
      />
      <path
        d="M5 21a5 5 0 0 1 5-5h24l7 5h30a5 5 0 0 1 5 5v29a5 5 0 0 1-5 5H10a5 5 0 0 1-5-5Z"
        fill="#f2c54b"
      />
      <path
        d="M10 22h61a4 4 0 0 1 4 4v29a4 4 0 0 1-4 4H10a4 4 0 0 1-4-4V26a4 4 0 0 1 4-4Z"
        fill="#ffcf52"
      />
    </svg>
  );
}

function FileIcon() {
  return (
    <svg className="fs-file-icon" viewBox="0 0 56 64" aria-hidden="true">
      <path d="M10 3h24l12 12v44H10Z" fill="#a9b8d0" />
      <path d="M34 3v12h12" fill="#e0e6ef" />
      <path d="M18 27h20M18 35h20M18 43h14" stroke="#62738c" strokeWidth="3" />
    </svg>
  );
}

export default function FileBrowser({
  initialPath,
  onSelect,
  onClose,
}: FileBrowserProps) {
  const dialogRef = useRef<HTMLDivElement>(null);
  const [locations, setLocations] = useState<FilesystemRoots | null>(null);
  const [history, setHistory] = useState<string[]>(
    initialPath ? [initialPath] : [],
  );
  const [historyIndex, setHistoryIndex] = useState(initialPath ? 0 : -1);
  const path = history[historyIndex] || "";
  const [address, setAddress] = useState(initialPath);
  const [search, setSearch] = useState("");
  const [query, setQuery] = useState("");
  const [listing, setListing] = useState<DirectoryListing | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [loadingMore, setLoadingMore] = useState(false);
  const [error, setError] = useState("");
  const [view, setView] = useState<"grid" | "list">("grid");
  const [folderName, setFolderName] = useState("");
  const [showNewFolder, setShowNewFolder] = useState(false);
  const [creatingFolder, setCreatingFolder] = useState(false);
  const [selecting, setSelecting] = useState(false);
  const [refresh, setRefresh] = useState(0);
  const requestSequence = useRef(0);
  const [rootsLoaded, setRootsLoaded] = useState(false);
  const filesystemBusy = creatingFolder || selecting;
  useDialogFocus(dialogRef, () => {
    if (!filesystemBusy) onClose();
  });

  useEffect(() => {
    let active = true;
    api<FilesystemRoots>("/filesystem/roots")
      .then((data) => {
        if (!active) return;
        setLocations(data);
        if (!initialPath) {
          setHistory([data.default_path]);
          setHistoryIndex(0);
          setAddress((current) =>
            current === "" ? data.default_path : current,
          );
        }
      })
      .catch((reason) => {
        if (active) setError(errorMessage(reason));
      })
      .finally(() => {
        if (active) {
          setRootsLoaded(true);
          if (!initialPath) setLoading(false);
        }
      });
    return () => {
      active = false;
    };
  }, [initialPath]);

  useEffect(() => {
    const timer = window.setTimeout(() => setQuery(search.trim()), 200);
    return () => window.clearTimeout(timer);
  }, [search]);

  useEffect(() => {
    if (!path) return;
    const sequence = ++requestSequence.current;
    let active = true;
    setLoading(true);
    setLoadingMore(false);
    setSelected(null);
    setError("");
    const parameters = new URLSearchParams({
      path,
      search: query,
      offset: "0",
      limit: "200",
    });
    api<DirectoryListing>("/filesystem/list?" + parameters)
      .then((data) => {
        if (!active || sequence !== requestSequence.current) return;
        setListing(data);
        setAddress((current) => (current === path ? data.path : current));
      })
      .catch((reason) => {
        if (!active || sequence !== requestSequence.current) return;
        setListing(null);
        setError(errorMessage(reason));
      })
      .finally(() => {
        if (active && sequence === requestSequence.current) setLoading(false);
      });
    return () => {
      active = false;
    };
  }, [path, query, refresh]);

  const navigate = (nextPath: string) => {
    if (!nextPath.trim()) return;
    setLoading(true);
    setSelected(null);
    setAddress(nextPath.trim());
    setSearch("");
    setQuery("");
    setShowNewFolder(false);
    setFolderName("");
    if (nextPath === path) {
      setRefresh((value) => value + 1);
      return;
    }
    setHistory([...history.slice(0, historyIndex + 1), nextPath.trim()]);
    setHistoryIndex(historyIndex + 1);
  };

  const goHistory = (index: number) => {
    setLoading(true);
    setSelected(null);
    setHistoryIndex(index);
    setAddress(history[index]);
    setSearch("");
    setQuery("");
    setShowNewFolder(false);
  };

  const loadMore = async () => {
    if (!listing || loadingMore || loading) return;
    const sequence = requestSequence.current;
    setLoadingMore(true);
    setError("");
    try {
      const parameters = new URLSearchParams({
        path: listing.path,
        search: query,
        offset: String(listing.entries.length),
        limit: "200",
      });
      const data = await api<DirectoryListing>(
        "/filesystem/list?" + parameters,
      );
      if (sequence === requestSequence.current) {
        setListing((previous) =>
          previous
            ? { ...data, entries: [...previous.entries, ...data.entries] }
            : data,
        );
      }
    } catch (reason) {
      if (sequence === requestSequence.current) setError(errorMessage(reason));
    } finally {
      if (sequence === requestSequence.current) setLoadingMore(false);
    }
  };

  const createFolder = async (event: React.FormEvent) => {
    event.preventDefault();
    if (!listing || !folderName.trim() || creatingFolder) return;
    setCreatingFolder(true);
    setError("");
    try {
      const created = await api<{ path: string }>(
        "/filesystem/folders",
        "POST",
        { parent_path: listing.path, name: folderName.trim() },
      );
      setShowNewFolder(false);
      setFolderName("");
      navigate(created.path);
    } catch (reason) {
      setError(errorMessage(reason));
    } finally {
      setCreatingFolder(false);
    }
  };

  const selectFolder = async () => {
    if (!listing || loading || filesystemBusy) return;
    setSelecting(true);
    setError("");
    try {
      await onSelect(selected || listing.path);
    } catch (reason) {
      setError(errorMessage(reason));
    } finally {
      setSelecting(false);
    }
  };

  const shortcuts = locations?.shortcuts || [];
  const roots =
    locations?.roots.filter(
      (root) => !shortcuts.some((shortcut) => shortcut.path === root.path),
    ) || [];
  const folderPath = selected || listing?.path || "";
  const isUnavailable = loading || !listing;

  return (
    <div
      className="project-dialog-overlay fs-overlay"
      onMouseDown={(event) => {
        if (event.target === event.currentTarget && !filesystemBusy) onClose();
      }}
    >
      <div
        className="fs-browser"
        data-testid="file-browser-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="fs-title"
        ref={dialogRef}
        tabIndex={-1}
      >
        <header className="project-dialog-heading">
          <h2 id="fs-title">Browse for folder</h2>
          <button
            className="project-dialog-close"
            aria-label="Close file browser"
            onClick={onClose}
            disabled={filesystemBusy}
          >
            ×
          </button>
        </header>
        <div className="fs-address-bar">
          <div className="fs-navigation">
            <button
              title="Back"
              aria-label="Back"
              disabled={historyIndex <= 0 || filesystemBusy}
              onClick={() => goHistory(historyIndex - 1)}
            >
              ←
            </button>
            <button
              title="Forward"
              aria-label="Forward"
              disabled={historyIndex >= history.length - 1 || filesystemBusy}
              onClick={() => goHistory(historyIndex + 1)}
            >
              →
            </button>
            <button
              title="Up one folder"
              aria-label="Up one folder"
              disabled={!listing?.parent_path || loading || filesystemBusy}
              onClick={() =>
                listing?.parent_path && navigate(listing.parent_path)
              }
            >
              ↑
            </button>
          </div>
          <form
            className="fs-path-form"
            onSubmit={(event) => {
              event.preventDefault();
              navigate(address);
            }}
          >
            <span aria-hidden="true">▱</span>
            <input
              aria-label="Folder path"
              data-dialog-autofocus
              value={address}
              onChange={(event) => setAddress(event.target.value)}
              placeholder="Enter a folder path"
              disabled={filesystemBusy}
            />
            <button
              title="Go to folder"
              aria-label="Go to folder"
              disabled={!address.trim() || filesystemBusy}
            >
              →
            </button>
          </form>
        </div>
        <div className="fs-workspace">
          <aside className="fs-sidebar" aria-label="Folder locations">
            <p className="fs-sidebar-heading">Quick access</p>
            {shortcuts.map((location) => (
              <button
                key={location.path}
                className={listing?.path === location.path ? "selected" : ""}
                disabled={filesystemBusy}
                onClick={() => navigate(location.path)}
                title={location.path}
              >
                <FolderIcon small />
                <span>{location.name}</span>
              </button>
            ))}
            {roots.length > 0 && (
              <p className="fs-sidebar-heading fs-roots-heading">
                This computer
              </p>
            )}
            {roots.map((location) => (
              <button
                key={location.path}
                className={listing?.path === location.path ? "selected" : ""}
                disabled={filesystemBusy}
                onClick={() => navigate(location.path)}
                title={location.path}
              >
                <span className="fs-drive-icon" aria-hidden="true">
                  ▤
                </span>
                <span>{location.name}</span>
              </button>
            ))}
          </aside>
          <main className="fs-main">
            <div className="fs-toolbar">
              <button
                className="project-dialog-button"
                disabled={isUnavailable || filesystemBusy}
                onClick={() => setShowNewFolder((visible) => !visible)}
              >
                <span aria-hidden="true">＋</span> New Folder
              </button>
              <div
                className="fs-view-toggle"
                role="group"
                aria-label="View options"
              >
                <button
                  title="Grid view"
                  aria-label="Grid view"
                  aria-pressed={view === "grid"}
                  onClick={() => setView("grid")}
                >
                  ▦
                </button>
                <button
                  title="List view"
                  aria-label="List view"
                  aria-pressed={view === "list"}
                  onClick={() => setView("list")}
                >
                  ☷
                </button>
              </div>
              <label className="fs-search">
                <span aria-hidden="true">⌕</span>
                <input
                  aria-label="Search folders and files"
                  value={search}
                  onChange={(event) => setSearch(event.target.value)}
                  placeholder="Search this folder"
                  maxLength={200}
                  disabled={filesystemBusy}
                />
              </label>
            </div>
            {showNewFolder && (
              <form className="fs-new-folder" onSubmit={createFolder}>
                <label htmlFor="fs-new-folder-name">Folder name</label>
                <input
                  id="fs-new-folder-name"
                  autoFocus
                  value={folderName}
                  onChange={(event) => setFolderName(event.target.value)}
                  maxLength={100}
                  disabled={filesystemBusy}
                />
                <button
                  className="project-dialog-button"
                  disabled={!folderName.trim() || filesystemBusy}
                >
                  {creatingFolder ? "Creating…" : "Create folder"}
                </button>
                <button
                  type="button"
                  aria-label="Cancel new folder"
                  disabled={filesystemBusy}
                  onClick={() => setShowNewFolder(false)}
                >
                  ×
                </button>
              </form>
            )}
            {error && (
              <p className="project-dialog-error" role="alert">
                {error}
              </p>
            )}
            <div
              className="fs-entries-scroll"
              aria-busy={loading || loadingMore}
            >
              {loading ? (
                <p className="fs-state-message" role="status">
                  Loading folders and files…
                </p>
              ) : !listing ? (
                <div className="fs-state-message">
                  <p>
                    {rootsLoaded
                      ? "Choose a location or enter a folder path to browse."
                      : "Loading locations…"}
                  </p>
                  <button
                    className="project-dialog-button"
                    onClick={() => setRefresh((value) => value + 1)}
                    disabled={!path}
                  >
                    Try again
                  </button>
                </div>
              ) : (
                <>
                  {listing.entries.length === 0 ? (
                    <p className="fs-state-message">
                      {query
                        ? "No matching folders or files."
                        : "This folder is empty."}
                    </p>
                  ) : (
                    <div
                      className={`fs-entries fs-${view}`}
                      role="list"
                      aria-label="Folders and files"
                    >
                      {listing.entries.map((entry) => (
                        <div
                          role="listitem"
                          key={`${entry.name}:${entry.path}`}
                        >
                          {entry.kind === "directory" ? (
                            <button
                              className={`fs-entry${selected === entry.path ? " selected" : ""}`}
                              data-testid="browser-entry"
                              data-entry-path={entry.path}
                              data-entry-kind="directory"
                              title={entry.path + " — double-click to open"}
                              aria-label={`Folder ${entry.name}`}
                              aria-pressed={selected === entry.path}
                              onClick={() => setSelected(entry.path)}
                              onDoubleClick={() => navigate(entry.path)}
                              onKeyDown={(event) => {
                                if (event.key === "Enter") {
                                  event.preventDefault();
                                  navigate(entry.path);
                                }
                              }}
                              disabled={filesystemBusy}
                            >
                              <FolderIcon small={view === "list"} />
                              <span className="fs-entry-name">
                                {entry.name}
                              </span>
                              <span className="fs-entry-meta">
                                {entry.modified_at
                                  ? new Date(entry.modified_at).toLocaleString(
                                      undefined,
                                      {
                                        dateStyle: "short",
                                        timeStyle: "short",
                                      },
                                    )
                                  : "Folder"}
                              </span>
                            </button>
                          ) : (
                            <div
                              className="fs-entry fs-file"
                              data-testid="browser-entry"
                              data-entry-path={entry.path}
                              data-entry-kind="file"
                              title={entry.path}
                              aria-label={`File ${entry.name}`}
                            >
                              <FileIcon />
                              <span className="fs-entry-name">
                                {entry.name}
                              </span>
                              <span className="fs-entry-meta">
                                {entry.size === null
                                  ? "File"
                                  : entry.size < 1024
                                    ? `${entry.size} B`
                                    : `${Math.ceil(entry.size / 1024).toLocaleString()} KB`}
                              </span>
                            </div>
                          )}
                        </div>
                      ))}
                    </div>
                  )}
                  {listing.entries.length < listing.total && (
                    <button
                      className="project-dialog-button fs-load-more"
                      disabled={loadingMore || filesystemBusy}
                      onClick={loadMore}
                    >
                      {loadingMore
                        ? "Loading…"
                        : `Load more (${listing.entries.length} of ${listing.total})`}
                    </button>
                  )}
                </>
              )}
            </div>
            <div className="fs-status" role="status">
              {listing && !loading
                ? `${listing.total.toLocaleString()} ${listing.total === 1 ? "item" : "items"}`
                : ""}
              <span>Double-click a folder to open it</span>
            </div>
          </main>
        </div>
        <footer className="fs-footer">
          <label className="fs-selection">
            <span>Folder</span>
            <input
              aria-label="Selected folder"
              readOnly
              value={isUnavailable ? "" : folderPath}
            />
          </label>
          <div className="project-dialog-actions">
            <button
              className="project-dialog-button"
              onClick={onClose}
              disabled={filesystemBusy}
            >
              Cancel
            </button>
            <button
              className="project-dialog-button primary"
              onClick={() => void selectFolder()}
              disabled={isUnavailable || filesystemBusy || !folderPath}
            >
              {selecting ? "Opening…" : "Select Folder"}
            </button>
          </div>
        </footer>
      </div>
    </div>
  );
}
