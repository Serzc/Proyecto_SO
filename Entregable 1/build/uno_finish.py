"""Abre un .docx con LibreOffice (UNO), actualiza el índice (TOC) y los campos,
guarda el .docx actualizado y exporta el PDF.
Uso: python3 uno_finish.py entrada.docx salida.docx salida.pdf"""
import sys, os, time, subprocess, uno
from com.sun.star.beans import PropertyValue

def prop(n, v):
    p = PropertyValue(); p.Name = n; p.Value = v; return p

src, out_docx, out_pdf = [os.path.abspath(a) for a in sys.argv[1:4]]
SOFFICE = r"C:\Program Files\LibreOffice\program\soffice.exe"   # Entregable 1: no está en PATH
proc = subprocess.Popen([SOFFICE, "--headless", "--invisible", "--nologo", "--norestore",
                         "--accept=socket,host=localhost,port=2002;urp;"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
local = uno.getComponentContext()
resolver = local.ServiceManager.createInstanceWithContext("com.sun.star.bridge.UnoUrlResolver", local)
for _ in range(60):
    try:
        ctx = resolver.resolve("uno:socket,host=localhost,port=2002;urp;StarOffice.ComponentContext"); break
    except Exception:
        time.sleep(0.5)
desktop = ctx.ServiceManager.createInstanceWithContext("com.sun.star.frame.Desktop", ctx)
doc = desktop.loadComponentFromURL(uno.systemPathToFileUrl(src), "_blank", 0, (prop("Hidden", True),))
for _ in range(2):
    idx = doc.getDocumentIndexes()
    for i in range(idx.getCount()):
        idx.getByIndex(i).update()
    doc.getTextFields().refresh()
    doc.refresh()
doc.storeToURL(uno.systemPathToFileUrl(out_docx), (prop("FilterName", "MS Word 2007 XML"),))
doc.close(True)
try: desktop.terminate()
except Exception: pass
proc.wait(timeout=30)
print("ok")
