# Development Rules

## Code Style
- No inline comments (code should be self-documenting)
- No docstrings unless public API
- Descriptive variable and function names
- Single responsibility principle
- Maximum function length: 30 lines

## Git
- Commit messages in English
- Format: `<type>: <description>`
- Types: feat, fix, refactor, test, docs
- No mentions of AI, Claude, or assistant
- Example: `feat: add dead reckoning position integration`

## Session Start Checklist
1. Read RULES.md
2. Read current plan file
3. Check git status and branch
4. Review recent changes

## Architecture
- Keep modules loosely coupled
- Use dependency injection
- Events for cross-module communication
- Config in JSON files, not hardcoded
