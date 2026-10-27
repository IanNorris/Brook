import {writeFile} from 'node:fs/promises';
export async function checkQmpInput(page,artifactDir){
 const send=events=>page.evaluate(events=>window.brook.brookMonitor.send('input-send-event',{events},'input-'+Date.now()),events);
 const size=await page.locator('#canvas').evaluate(c=>({width:c.width,height:c.height}));
 const rel=(axis,value)=>({type:'rel',data:{axis,value}}),button=down=>({type:'btn',data:{down,button:'left'}});
 // Fresh Brook PS/2 cursor starts at0,0. No DOM mouse events are sent.
 await send([rel('x',18),rel('y',size.height-18)]);await page.waitForTimeout(600);
 await send([button(true)]);await page.waitForTimeout(100);await send([button(false)]);
 await page.waitForTimeout(1000);
 await page.locator('#canvas').screenshot({path:`${artifactDir}/qmp-launcher-open.png`,timeout:5000});
 await send([button(true)]);await page.waitForTimeout(100);await send([button(false)]);
 await page.waitForTimeout(700);
 await page.locator('#canvas').screenshot({path:`${artifactDir}/qmp-launcher-closed.png`,timeout:5000});
 const key=(name,down)=>({type:'key',data:{down,key:{type:'qcode',data:name}}});
 await send([key('shift',true),key('shift',false),key('shift',true),key('shift',false)]);
 await send([key('meta_l',true),key('ret',true),key('ret',false),key('meta_l',false)]);
 await page.waitForTimeout(2500);
 const log=await page.locator('#log').textContent();
 const result={keyboardHotkeyObserved:/TERMINAL: bash not found|WM: failed to spawn new terminal/.test(log),mouseVisualAcceptance:'Review qmp-launcher-open/closed images',canvas:size,transport:'QMP only; no DOM input'};
 await writeFile(`${artifactDir}/input-log.txt`,log);
 return result;
}
