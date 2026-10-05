import Markdown from "react-markdown";
import remarkGfm from "remark-gfm";

function scanLine(line: string, initialTicks = 0) {
  const pipes: number[] = [];
  let ticks = initialTicks;
  let pipeInCode = false;
  for (let index = 0; index < line.length; index++) {
    if (!ticks && line[index] === "\\") {
      index++;
      continue;
    }
    if (line[index] === "`") {
      let end = index + 1;
      while (line[end] === "`") end++;
      const length = end - index;
      if (!ticks) ticks = length;
      else if (ticks === length) ticks = 0;
      index = end - 1;
    } else if (line[index] === "|") {
      if (ticks) pipeInCode = true;
      else pipes.push(index);
    }
  }
  return { pipes, ticks, pipeInCode };
}

function tableCells(row: string) {
  const { pipes, ticks, pipeInCode } = scanLine(row);
  if (ticks || pipeInCode || pipes[0] !== 0 || pipes.at(-1) !== row.length - 1)
    return null;
  const cells = pipes
    .slice(1)
    .map((end, index) => row.slice(pipes[index] + 1, end).trim());
  return cells.every(Boolean) ? cells : null;
}

function restoreTableRows(line: string) {
  const trimmed = line.trim();
  if (!trimmed.startsWith("|") || !trimmed.endsWith("|")) return line;
  const { pipes, ticks, pipeInCode } = scanLine(trimmed);
  if (ticks || pipeInCode) return line;

  const rows: string[] = [];
  let start = 0;
  for (let index = 0; index < pipes.length - 1; index++) {
    // Two outer pipes with only whitespace between them mark a lost row break.
    if (/^[ \t]*$/.test(trimmed.slice(pipes[index] + 1, pipes[index + 1]))) {
      rows.push(trimmed.slice(start, pipes[index] + 1));
      start = pipes[index + 1];
    }
  }
  rows.push(trimmed.slice(start));
  if (rows.length < 3) return line;

  const cells = rows.map(tableCells);
  const header = cells[0];
  const separator = cells[1];
  const separatorCell = /^:?-{3,}:?$/;
  if (
    !header ||
    header.length < 2 ||
    header.every((cell) => separatorCell.test(cell)) ||
    !separator ||
    !separator.every((cell) => separatorCell.test(cell)) ||
    cells.some((row) => !row || row.length !== header.length) ||
    cells.slice(2).some((row) => row?.every((cell) => separatorCell.test(cell)))
  )
    return line;

  const indent = line.match(/^ */)?.[0] || "";
  return rows.map((row) => indent + row).join("\n");
}

/** Restore only unambiguous flattened pipe tables, keeping source code untouched. */
export function normalizeMarkdownTables(text: string) {
  let fence: { marker: string; length: number } | null = null;
  let inlineTicks = 0;
  return text
    .split(/(\r?\n)/)
    .map((line, index) => {
      if (index % 2) return line;
      if (fence) {
        if (
          new RegExp(`^ {0,3}${fence.marker}{${fence.length},}[ \\t]*$`).test(
            line,
          )
        )
          fence = null;
        return line;
      }
      const opening = !inlineTicks && line.match(/^ {0,3}(`{3,}|~{3,})/);
      if (opening) {
        fence = { marker: opening[1][0], length: opening[1].length };
        return line;
      }
      if (!inlineTicks && /^( {4}|\t)/.test(line)) return line;
      const previousTicks = inlineTicks;
      inlineTicks = scanLine(line, inlineTicks).ticks;
      return previousTicks || inlineTicks ? line : restoreTableRows(line);
    })
    .join("");
}

export default function MarkdownMessage({ text }: { text: string }) {
  return (
    <div className="markdown-message">
      <Markdown
        skipHtml
        remarkPlugins={[remarkGfm]}
        components={{
          img: () => null,
          a: ({ children, href }) => (
            <a href={href} target="_blank" rel="noreferrer noopener">
              {children}
            </a>
          ),
          table: ({ children }) => (
            <div
              className="markdown-table-scroll"
              role="region"
              aria-label="Message table"
              tabIndex={0}
            >
              <table>{children}</table>
            </div>
          ),
          th: ({ children, ...props }) => {
            const { node: _node, ...attributes } = props;
            return (
              <th {...attributes} scope="col">
                {children}
              </th>
            );
          },
        }}
      >
        {normalizeMarkdownTables(text)}
      </Markdown>
    </div>
  );
}
