#!/usr/bin/env python3
"""Construye el Entregable 1 (adaptado del build.py de Tarea 3):
diseno.md -> (preprocesado) -> pandoc -> docx -> LibreOffice (índice actualizado) -> docx final + PDF.

Preprocesado de ../diseno.md:
  - quita el comentario inicial, la sección "Portada" (se reemplaza por la portada en OpenXML con el
    Abstract y el índice) y todo lo que sigue al marcador "NO VA EN EL ENTREGABLE" (Pendientes);
  - cada bloque ```mermaid seguido de ": Figura N. ..." se cambia por la imagen figures/*-mermaid.png.

Uso (Windows): python build.py
Importante: después de la primera construcción el .docx es la fuente de verdad (CLAUDE.md, sección 4);
los cambios posteriores se hacen sobre el .docx con python-docx, no volviendo a correr este script.
"""
import re, sys, subprocess, html, os
from pathlib import Path

BUILD = Path(__file__).resolve().parent
ROOT = BUILD.parent                                   # Proyecto\Entregable 1
NAME = "SO.2026 - Proyecto Entregable1 - SergioZuniga JoseMadriz"
PANDOC = os.environ.get("PANDOC", str(Path(os.environ["LOCALAPPDATA"]) / "Pandoc" / "pandoc.exe"))
SOFFICE = r"C:\Program Files\LibreOffice\program\soffice.exe"
LO_PYTHON = r"C:\Program Files\LibreOffice\program\python.exe"

FIGS = [  # (archivo, atributos de tamaño), en el orden de los bloques mermaid
    ("fig1-arquitectura-mermaid.png", "width=6.5in"),
]


def p(text, size=22, bold=False, before=0, after=0, jc="center"):
    rpr = ("<w:b/>" if bold else "") + f'<w:sz w:val="{size}"/>'
    return (f'<w:p><w:pPr><w:spacing w:before="{before}" w:after="{after}"/><w:jc w:val="{jc}"/></w:pPr>'
            f'<w:r><w:rPr>{rpr}</w:rPr><w:t xml:space="preserve">{html.escape(text, quote=False)}</w:t></w:r></w:p>')


def lp(label, value, before=0):
    return (f'<w:p><w:pPr><w:spacing w:before="{before}" w:after="0"/><w:jc w:val="center"/></w:pPr>'
            f'<w:r><w:rPr><w:b/><w:sz w:val="22"/></w:rPr><w:t xml:space="preserve">{label} </w:t></w:r>'
            f'<w:r><w:rPr><w:sz w:val="22"/></w:rPr><w:t xml:space="preserve">{html.escape(value, quote=False)}</w:t></w:r></w:p>')


def cover(abstract):
    x = [
        p("Instituto Tecnológico de Costa Rica", 26, True, before=200),
        p("Campus Tecnológico Central Cartago"),
        p("Escuela de Ingeniería en Computación"),
        lp("Curso:", "IC-6600 Principios de Sistemas Operativos"),
        lp("Profesor:", "M.Sc. Esteban Arias Méndez"),
        p("Proyecto Programado", 40, True, before=700),
        p("Entregable 1: Documento de Diseño del Sistema DSM", 30, before=60),
        p("Implementación de un Sistema de Memoria Compartida Distribuida (DSM) con Paginación bajo "
          "Demanda y Coherencia MSI", 22, before=120),
        p("Estudiantes:", 22, True, before=500),
        p("Sergio Andrés Zúñiga Castillo, carné 2024146884"),
        p("José Ignacio Madriz Ortiz, carné 2024089190"),
        lp("Periodo:", "II Semestre 2026", before=200),
        lp("Fecha de entrega:", "Noviembre 6, 2026"),
        p("Abstract", 22, True, before=400, after=80, jc="left"),
        p(abstract, 19, jc="both"),
        '<w:p><w:r><w:br w:type="page"/></w:r></w:p>',
        '<w:sdt><w:sdtPr><w:docPartObj><w:docPartGallery w:val="Table of Contents"/><w:docPartUnique/>'
        '</w:docPartObj></w:sdtPr><w:sdtContent><w:p><w:pPr><w:pStyle w:val="TOCHeading"/></w:pPr><w:r>'
        '<w:t>Índice</w:t></w:r></w:p><w:p><w:r><w:fldChar w:fldCharType="begin" w:dirty="true"/></w:r><w:r>'
        '<w:instrText xml:space="preserve"> TOC \\o "1-2" \\h \\z \\u </w:instrText></w:r><w:r>'
        '<w:fldChar w:fldCharType="separate"/></w:r><w:r><w:t>Índice (actualizar campo)</w:t></w:r><w:r>'
        '<w:fldChar w:fldCharType="end"/></w:r></w:p></w:sdtContent></w:sdt>',
        '<w:p><w:r><w:br w:type="page"/></w:r></w:p>',
    ]
    return "```{=openxml}\n" + "\n".join(x) + "\n```\n\n"


def preprocess(md):
    md = re.sub(r"\A<!--.*?-->\s*", "", md, flags=re.S)
    cut = md.find("<!-- ======================== NO VA EN EL ENTREGABLE")
    if cut < 0:
        sys.exit("no se encontró el marcador de Pendientes")
    md = md[:cut].rstrip() + "\n"
    m = re.search(r"\*\*Abstract\*\*\s*\n\n(.+?)\n\n", md, re.S)
    if not m:
        sys.exit("no se encontró el Abstract")
    abstract = m.group(1).strip()
    start = md.find("# 1. Introducción")
    md = cover(abstract) + md[start:]

    figs = iter(FIGS)
    def fig(mt):
        name, size = next(figs)
        cap = mt.group(1).replace(" (borrador)", "")
        return f"![{cap}](figures/{name}){{{size}}}\n"
    md, n = re.subn(r"```mermaid\n.*?```\s*\n: (Figura \d+\..*?)\n", fig, md, flags=re.S)
    if n != len(FIGS):
        sys.exit(f"se esperaban {len(FIGS)} figuras y se encontraron {n}")
    return md


def main():
    md = preprocess((ROOT / "diseno.md").read_text(encoding="utf-8"))
    src = BUILD / "diseno_expandido.md"   # archivo intermedio generado
    src.write_text(md, encoding="utf-8")
    for ch in ["\u2014", "\u2013"]:
        if ch in md:
            print(f"AVISO: el texto contiene el carácter {ch!r}")
    tmp = BUILD / "tmp_pandoc.docx"
    subprocess.run([PANDOC, str(src), "-o", str(tmp), "--from", "markdown-smart",   # sin comillas curvas, puntos suspensivos ni espacios duros
"--reference-doc", str(BUILD / "reference.docx"), "--syntax-highlighting=tango",
                    "--resource-path", str(ROOT)], check=True)
    final_docx = ROOT / f"{NAME}.docx"
    subprocess.run([LO_PYTHON, str(BUILD / "uno_finish.py"), str(tmp), str(final_docx), "x.pdf"], check=True)
    subprocess.run([SOFFICE, "--headless", "--convert-to", "pdf", "--outdir", str(ROOT), str(final_docx)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tmp.unlink(missing_ok=True)
    print("listo:", final_docx, ROOT / f"{NAME}.pdf")


if __name__ == "__main__":
    main()
