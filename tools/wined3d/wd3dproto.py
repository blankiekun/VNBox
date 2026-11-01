"""Parse wined3d.h: prototypes of the __cdecl wined3d_* API and which structs are complete (have a body here).
Shared by the thunk generator (gen-thunk.py) and its checks."""
import re

def strip_comments(src):
    return re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)

def complete_structs(src):
    return set(re.findall(r'\bstruct\s+(wined3d_\w+)\s*\{', src))

def enums(src):
    return set(re.findall(r'\benum\s+(wined3d_\w+)\s*\{', src))

class Param:
    def __init__(self, text):
        self.text = ' '.join(text.split())
        m = re.match(r'(.*?)(\w+)\s*(\[\s*\w*\s*\])?$', self.text)
        self.type = ' '.join(m.group(1).replace('*', ' * ').split()).replace(' * ', '*').replace(' *', '*')
        self.name = m.group(2)
        self.array = m.group(3)
        if self.array:
            self.type += '*'
        self.ptr = self.type.count('*')
        self.const = self.type.startswith('const ')
        base = self.type.replace('const ', '').replace('*', '').strip()
        self.base = base

class Proto:
    def __init__(self, ret, name, params):
        self.ret = ' '.join(ret.split())
        self.name = name
        self.params = [Param(p) for p in params if p.strip() and p.strip() != 'void']

def prototypes(src):
    src = strip_comments(src)
    out = {}
    for m in re.finditer(r'([A-Za-z_][\w\s\*]*?)\s*__cdecl\s+(wined3d_\w+)\s*\(([^;{]*?)\)\s*;', src):
        ret, name, args = m.group(1).strip(), m.group(2), m.group(3)
        ret = ret.split('\n')[-1].strip()
        out[name] = Proto(ret, name, [a for a in args.split(',')])
    return out
