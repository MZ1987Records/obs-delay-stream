# OBS Native Plugin UI Review Checklist

- Defaults declared for all persistent settings
- `get_properties()` constructs UI without owning persistent state
- No cached `obs_property_t*` or `obs_properties_t*`
- `modified_callback` performs only lightweight UI work
- Callback returns `true` only for actual redraw / layout refresh
- **Every `return true` modified callback schedules ALL inject types for its tab** (RefreshProperties does NOT call get_properties — see mvvm-architecture SKILL.md §CB ルール)
- Runtime state applied in `update()` or owned apply path
- Dynamic lists persist stable IDs
- Hidden-field behavior intentional and documented
- UI-only state separated from runtime config
- `data == NULL` assumptions audited
- Settings migration covered when schema evolved
- Injected widget destructors do not erase binding_id map entries (stale entries cleaned at registration)
- `schedule_widget_injects_for_tab()`, modified callback 本体, スキルの inject 一覧表が同期している
