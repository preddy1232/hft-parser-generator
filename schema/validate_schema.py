"""Validate itch50_schema.json: JSON well-formedness, offset continuity, size correctness."""
import json
import sys

def validate(path):
    with open(path, 'r') as f:
        schema = json.load(f)
    
    print(f"Protocol: {schema['protocol']}")
    print(f"Messages: {len(schema['messages'])}")
    print(f"Data Types: {len(schema['data_types'])}")
    print()
    
    errors = 0
    for msg in schema['messages']:
        name = msg['name']
        declared_size = msg['size']
        msg_type = msg['type']
        fields = msg['fields']
        
        # Check fields are contiguous and sum to declared size
        expected_offset = 0
        for field in fields:
            if field['offset'] != expected_offset:
                print(f"  ERROR: {name}.{field['name']} offset={field['offset']} expected={expected_offset}")
                errors += 1
            expected_offset = field['offset'] + field['length']
        
        computed_size = expected_offset
        status = "OK" if computed_size == declared_size else "MISMATCH"
        if status != "OK":
            errors += 1
        
        print(f"  [{msg_type}] {name:35s}  declared={declared_size:3d}  computed={computed_size:3d}  fields={len(fields):2d}  {status}")
    
    print()
    if errors == 0:
        print("ALL CHECKS PASSED")
    else:
        print(f"FAILED: {errors} error(s)")
    return errors

if __name__ == '__main__':
    sys.exit(validate(sys.argv[1] if len(sys.argv) > 1 else 'schema/itch50_schema.json'))
