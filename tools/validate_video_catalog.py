#!/usr/bin/env python3
"""Validate video catalogues without starting the emulator."""
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, f'{path}: duplicate key {key}')
            result[key] = value
        return result
    return json.loads(Path(path).read_text(), object_pairs_hook=pairs)


def index(items, label):
    result = {}
    for item in items:
        code = item['code']
        require(isinstance(code, str) and code and code not in result,
                f'{label}: invalid or duplicate code {code}')
        result[code] = item
    return result


def validate(devices, capabilities, profiles):
    require(devices['schema_version'] == 2, 'devices: unsupported schema')
    require(capabilities['schema_version'] == 1, 'capabilities: unsupported schema')
    require(profiles['schema_version'] == 2, 'profiles: unsupported schema')
    ds = index(devices['devices'], 'devices')
    variants = {}
    for code, device in ds.items():
        vs = index(device['variants'], code)
        require(not variants.keys() & vs.keys(), 'duplicate variant across devices')
        variants.update(vs)
        cores = device['cores']
        require(len(cores) == len(set(cores)), f'{code}: duplicate core')
        require(set(cores) == set(device['core_variants']), f'{code}: core support mismatch')
        for core, supported in device['core_variants'].items():
            require(supported and len(supported) == len(set(supported)), f'{code}: invalid support list')
            require(all(v in vs and vs[v]['status'] == 'implemented' for v in supported),
                    f'{code}: core references unavailable variant')
        if cores:
            require(device['default_core'] in cores, f'{code}: invalid default core')
            require(device['default_variant'] in device['core_variants'][device['default_core']],
                    f'{code}: invalid default variant')
        else:
            require(device['default_core'] is None and device['default_variant'] is None,
                    f'{code}: unavailable device has defaults')
        for v in vs.values():
            require(v['status'] in ('implemented', 'planned', 'not_implemented'), 'invalid status')
            f = v['frame']
            require(f['column_unit'] in ('dot', 'pio_tick', 'pixel_clock', None), 'invalid raster unit')
            known = f['columns'] is not None or f['rows'] is not None
            if known:
                require(all(type(f[k]) is int and f[k] > 0 for k in ('columns', 'rows')) and
                        f['column_unit'] is not None, 'invalid raster dimensions')
            if v['status'] == 'implemented':
                require(known and isinstance(v['density_factor'], (int, float)) and v['density_factor'] > 0,
                        'implemented variant lacks geometry')
                require(any(v['code'] in a for a in device['core_variants'].values()),
                        'implemented variant lacks core')
    caps = index(capabilities['capabilities'], 'capabilities')
    shaders = index(capabilities['shaders'], 'shaders')
    values = {k: index(c['values'], k) for k, c in caps.items()}
    for choices in values.values():
        for v in choices.values():
            require('shader' not in v or v['shader'] in shaders, 'unknown shader')
    for shader in shaders.values():
        names = set()
        for p in shader['parameters']:
            require(p['key'] not in names, 'duplicate shader parameter')
            names.add(p['key'])
            if p['type'] == 'number':
                require(p['min'] <= p['default'] <= p['max'], 'invalid parameter default')
            elif p['type'] == 'enum':
                index(p['values'], 'shader values')
                require(p['default'] in [v['value'] for v in p['values']], 'invalid enum default')
            else:
                raise ValueError('unknown parameter type')
    for name, profile in profiles['profiles'].items():
        for cap, allowed in profile['capabilities'].items():
            require(cap in values and allowed and len(allowed) == len(set(allowed)) and
                    set(allowed) <= values[cap].keys(), f'{name}: unknown capability/value')
        for context, defaults in profile['defaults'].items():
            require(context in ('computer', 'debug'), 'unknown context')
            for cap, value in defaults.items():
                require(value in profile['capabilities'].get(cap, []), 'default outside profile')
                require(context in caps[cap].get('contexts', [context]) and
                        context in values[cap][value].get('contexts', [context]), 'default outside context')
        for override in profile.get('overrides', []):
            when = override['when']
            require(when['value'] in profile['capabilities'].get(when['capability'], []), 'invalid condition')
            for cap, allowed in override['capabilities'].items():
                require(set(allowed) <= set(profile['capabilities'].get(cap, [])), 'override expands values')
        require(profile['default_crop'] in profile['crop_regions'], 'invalid default crop')
        for region in profile['crop_regions'].values():
            require(region['source'] in ('visible_area', 'content_area'), 'unknown frame region')
    require(set(profiles['variants']) == set(variants), 'incomplete variant profiles')
    for variant, name in profiles['variants'].items():
        require(name is None or name in profiles['profiles'], 'unknown profile')
        require((name is not None) == (variants[variant]['status'] == 'implemented'), 'profile availability mismatch')
    return ds, variants


def load(root):
    root = Path(root)
    return tuple(read_json(root / n) for n in
                 ('video_devices.json', 'video_output_capabilities.json', 'video_output_profiles.json'))



def validate_machine_options(data, devices, profiles):
    require(data['schema_version'] == 1, 'machine options: unsupported schema')
    ds = index(devices['devices'], 'devices')
    options = index(data['options'], 'options')
    machines = index(data['machines'], 'machines')
    ids = set()
    bindings = set()
    for option in options.values():
        require(option['device'] in ds, 'unknown option device')
        d = ds[option['device']]
        require(option['variant'] in d['core_variants'].get(option['core'], []), 'unsupported option combination')
        require(option['output_profile'] in profiles['profiles'], 'unknown option profile')
        require(option['legacy_device'] in ('TMS9918','TMS9918B','TMS9928','TMS9929','Pico9918','PicoSMSVdp','SegaSMSVdpNTSC','SegaGameGearVdp'), 'unknown factory binding')
        require(option['legacy_device'] not in bindings, 'duplicate legacy binding')
        bindings.add(option['legacy_device'])
        require(type(option['legacy_config_value']) is int and 1 <= option['legacy_config_value'] <= 8, 'invalid legacy config')
    require(len({o['legacy_config_value'] for o in options.values()}) == len(options), 'duplicate legacy config')
    for machine in machines.values():
        require(type(machine['model_id']) is int and machine['model_id'] not in ids, 'duplicate model id')
        ids.add(machine['model_id'])
        choices = machine['options']
        require(choices and len(choices) == len(set(choices)) and set(choices) <= options.keys(), 'invalid machine options')
        require(machine['default_option'] in choices, 'default outside machine')
    require(ids == set(range(17)), 'missing machine models')
    return options, machines


if __name__ == '__main__':
    import sys
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'src'
    data = load(root)
    ds, vs = validate(*data)
    options, machines = validate_machine_options(read_json(root / 'video_machine_options.json'), data[0], data[2])
    print(f'Validated {len(ds)} devices, {len(vs)} variants, {len(options)} options and {len(machines)} models')
