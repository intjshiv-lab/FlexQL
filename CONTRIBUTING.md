# Contributing to FlexQL

Thanks for considering a contribution.

## Getting Started

1. **Fork** the repository
2. **Clone** your fork locally
3. **Build** the project (see [README.md](README.md) for instructions)
4. **Create a branch** for your feature or bugfix

## Building & Testing

```bash
cd flexql
make clean && make all        # Build everything
make run-tests                # Run all 71 unit tests
make benchmark                # Run 10M-row benchmark
```

## Code Style

- **C++17** standard
- **4-space indentation**, no tabs
- Header guards: `#ifndef FLEXQL_MODULE_H`
- Keep functions short and focused
- Every public function must have a brief comment explaining purpose

## Pull Request Process

1. Ensure all **71 tests pass** before submitting
2. Add tests for any new functionality
3. Update documentation if you change APIs
4. Write clear commit messages (imperative mood: "Add feature", not "Added feature")
5. One feature per PR — keep PRs small and reviewable

## Reporting Issues

- Use GitHub Issues
- Include: OS, compiler version, steps to reproduce, expected vs actual behavior
- For performance issues, include benchmark results

## Architecture

Please read [`docs/DesignDoc.md`](docs/DesignDoc.md) before making significant changes. The document explains all design decisions and their rationale.

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
