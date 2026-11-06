import assert from 'node:assert/strict'
import {readFileSync,readdirSync} from 'node:fs'
import {resolve} from 'node:path'
import {fileURLToPath} from 'node:url'
import {test} from 'node:test'
const root=fileURLToPath(new URL('../src/',import.meta.url))
function files(dir){return readdirSync(dir,{withFileTypes:true}).flatMap(entry=>entry.isDirectory()?files(resolve(dir,entry.name)):[resolve(dir,entry.name)])}
test('desktop feature views do not import mobile UI or select a mobile branch',()=>{
 for(const name of ['overview/Dashboard','conversation/GroupManager','conversation/PeopleManager','diagnostics/LogViewer']){
  const source=readFileSync(resolve(root,'features',name+'.vue'),'utf8')
  assert.doesNotMatch(source,/from\s+['"][^'"]*\/mobile\//,name)
  assert.doesNotMatch(source,/defineProps<\{mobile\?|v-(?:else-)?if="mobile"/,name)
 }
})
test('shared feature controllers contain no view imports',()=>{
 for(const file of files(resolve(root,'shared/features'))){assert.doesNotMatch(readFileSync(file,'utf8'),/from\s+['"][^'"]*\.vue['"]/,file)}
})
test('independent mobile page entries use shared controllers and never desktop features',()=>{
 for(const file of files(resolve(root,'mobile/pages'))){const source=readFileSync(file,'utf8');assert.match(source,/shared\/features\//,file);assert.doesNotMatch(source,/from\s+['"]\.\.\/\.\.\/features\//,file)}
 const app=readFileSync(resolve(root,'App.vue'),'utf8')
 assert.match(app,/<DesktopApp/)
 assert.match(app,/<MobileApp/)
 assert.doesNotMatch(app,/const mobilePages|:mobile="true"/)
})
