# OBS Native Plugin UI Review Checklist

- Defaults declared for all persistent settings
- `get_properties()` constructs UI without owning persistent state
- No cached `obs_property_t*` or `obs_properties_t*`
- `modified_callback` performs only lightweight UI work
- Callback returns `true` only for actual redraw / layout refresh
- Runtime state applied in `update()` or owned apply path
- Dynamic lists persist stable IDs
- Hidden-field behavior intentional and documented
- UI-only state separated from runtime config
- `data == NULL` assumptions audited
- Settings migration covered when schema evolved
