import {appendFile} from 'node:fs/promises';
/** Input acceptance targets only the emulator canvas in the owned browser. */
export async function checkGuestInput(page,artifactDir){
 const step=async name=>appendFile(`${artifactDir}/input-steps.log`,name+'\n');
 await step('bounds');
 const canvas=page.locator('#canvas'),box=await canvas.boundingBox();
 await step('canvas-size');
 const size=await canvas.evaluate(c=>({width:c.width,height:c.height}));
 if(!box)throw new Error('Guest canvas has no bounds');
 const point=(x,y)=>({x:box.x+x*box.width/size.width,y:box.y+y*box.height/size.height});
 const center=point(size.width/2,size.height/2),apps=point(18,size.height-18);
 await step('mouse-center');
 await page.mouse.move(center.x,center.y);
 await step('mouse-apps');
 await page.mouse.move(apps.x,apps.y,{steps:12});
 await step('mouse-click');
 await page.mouse.click(apps.x,apps.y);
 await page.waitForTimeout(1000);
 await canvas.screenshot({path:`${artifactDir}/input-launcher-open.png`,timeout:5000});
 await step('mouse-click');
 await page.mouse.click(apps.x,apps.y);
 await page.waitForTimeout(800);
 await canvas.screenshot({path:`${artifactDir}/input-launcher-closed.png`,timeout:5000});
 // Consume any initial PS/2 settling events with harmless modifier presses.
 await page.keyboard.press('Shift');await page.keyboard.press('Shift');
 await page.keyboard.press('Meta+Enter');
 await page.waitForTimeout(2500);
 const log=await page.locator('#log').textContent();
 const keyboard=/TERMINAL: bash not found|WM: failed to spawn new terminal/.test(log);
 return {keyboardHotkeyObserved:keyboard,mouseVisualAcceptance:'Review open/closed launcher images',canvas:size};
}
