# Secrets audit

Audited 2026-10-04. No credential values are recorded here.

The source-tree pattern scan covers Python, React, configuration, scripts,
documentation, the supplied skills, and preserved C++ sources. Generated builds,
dependencies, Git internals, and private runtime .env files are excluded.
No confirmed live credential was found.

| Location | Line | Category | Classification / action |
| --- | ---: | --- | --- |
| `original-project/the_connector_studio/tests/connector_tests.cpp` | 48 | Credential-like URL | Intentional invalid-URL rejection fixture; preserved |
| `original-project/the_connector_studio/tests/mcp_tests.cpp` | 281 | Credential-like URL | Intentional invalid MCP URL fixture; preserved |
| `skill/productionization/SKILL.md` | 683 | Provider-token-like text | Documentation example demonstrating unsafe credential storage; preserved |
| `skill/productionization/skills.md` | 683 | Provider-token-like text | Duplicate documentation example; preserved |

Reviewed examples are recorded by location and SHA-256 line fingerprint in
`scripts/reviewed_secret_examples.json`. A changed line is reviewed again.
The audit script emits categories and locations only and fails for unreviewed
findings. No original code was redacted because the matches are test/example
values, rather than live credentials.

Runtime credentials are read from environment variables, excluded by .gitignore,
kept out of API responses, and never included in Connector failure details.
The .env.example template contains an empty credential field.
Frontend npm audit reported zero vulnerabilities during setup.

This is a scoped working-tree pattern audit, not a guarantee that every possible
secret has been identified. No Git history rewrite or credential rotation was
needed based on these findings. Rerun `scripts/audit_secrets.py` when source changes.
