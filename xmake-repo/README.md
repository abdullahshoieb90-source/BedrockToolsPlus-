Vendored subset of [xmake-repo](https://github.com/xmake-io/xmake-repo)
package recipes used by this project (cmake, ninja, entt, fmt, glm,
nlohmann_json).

GitHub Actions often returns HTTP 403 when cloning the full upstream
repository. Registering this directory with `add_repositories` lets
`xmake f` resolve those packages without that clone.
