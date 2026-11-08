import {readFileSync} from 'node:fs'
import {createRequire} from 'node:module'
import {dirname, resolve} from 'node:path'
import {compileScript, parse} from 'vue/compiler-sfc'
import ts from 'typescript'
import * as vue from 'vue'

// Compile real setup code; child component views are stubbed for DOM-free tests.
export function createModuleLoader() {
  const require = createRequire(import.meta.url)
  const modules = new Map()
  function load(path) {
    if (modules.has(path)) return modules.get(path).exports
    const module = {exports: {}}
    modules.set(path, module)
    const source = readFileSync(path, 'utf8')
    const code = path.endsWith('.vue') ? compileScript(parse(source).descriptor, {id: path}).content : source
    const {outputText} = ts.transpileModule(code, {
      compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020}
    })
    new Function('require', 'module', 'exports', outputText)(name => {
      if (name === 'vue') return vue
      if (!name.startsWith('.')) return require(name)
      if (name.endsWith('.vue')) return {default: {render: () => null}}
      return load(resolve(dirname(path), `${name}.ts`))
    }, module, module.exports)
    if (path.endsWith('.vue')) module.exports.default.render = () => null
    return module.exports
  }
  return load
}

export const renderer = vue.createRenderer({
  createElement: () => ({}), createText: () => ({}), createComment: () => ({}),
  insert() {}, remove() {}, setText() {}, setElementText() {}, patchProp() {},
  parentNode: () => null, nextSibling: () => null
})
