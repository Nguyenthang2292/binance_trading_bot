from __future__ import annotations

import logging
from importlib import import_module
import queue
import threading
from pathlib import Path
from typing import Any, Callable, cast

from dotenv import load_dotenv

from .utils.pdf_tools import extract_pdf_page_range
from .utils.pdf_tools_gemini import convert_pdf_to_markdown_gemini

dpg = cast(Any, import_module("dearpygui.dearpygui"))


class SmallPdfApp:
    def __init__(self) -> None:
        self._ui_queue: queue.Queue[Callable[[], None]] = queue.Queue()

        self._tag_cut_input = "cut.input_pdf"
        self._tag_cut_start = "cut.start_page"
        self._tag_cut_end = "cut.end_page"
        self._tag_cut_output = "cut.output_dir"
        self._tag_cut_dialog_pdf = "cut.dialog_pdf"
        self._tag_cut_dialog_output = "cut.dialog_output"

        self._tag_convert_input = "convert.input_pdf"
        self._tag_convert_output = "convert.output_dir"
        self._tag_convert_dialog_pdf = "convert.dialog_pdf"
        self._tag_convert_dialog_output = "convert.dialog_output"
        self._tag_convert_progress = "convert.progress"
        self._tag_convert_status = "convert.status"
        self._tag_convert_result = "convert.result"
        self._convert_button_tags = ("convert.btn_gemini",)

    def run(self) -> None:
        dpg.create_context()
        dpg.configure_app(manual_callback_management=True)
        self._build_ui()
        dpg.create_viewport(title="Small PDF (DearPyGui)", width=980, height=560)
        dpg.setup_dearpygui()
        dpg.show_viewport()

        while dpg.is_dearpygui_running():
            jobs = dpg.get_callback_queue()
            dpg.run_callbacks(jobs)
            self._drain_ui_queue()
            dpg.render_dearpygui_frame()
        dpg.destroy_context()

    def _build_ui(self) -> None:
        with dpg.file_dialog(
            directory_selector=False,
            show=False,
            callback=lambda _s, app_data, _u: self._on_choose_path(self._tag_cut_input, app_data),
            tag=self._tag_cut_dialog_pdf,
            width=700,
            height=420,
        ):
            dpg.add_file_extension(".pdf")

        with dpg.file_dialog(
            directory_selector=True,
            show=False,
            callback=lambda _s, app_data, _u: self._on_choose_path(self._tag_cut_output, app_data),
            tag=self._tag_cut_dialog_output,
            width=700,
            height=420,
        ):
            dpg.add_file_extension(".*")

        with dpg.file_dialog(
            directory_selector=False,
            show=False,
            callback=lambda _s, app_data, _u: self._on_choose_path(self._tag_convert_input, app_data),
            tag=self._tag_convert_dialog_pdf,
            width=700,
            height=420,
        ):
            dpg.add_file_extension(".pdf")

        with dpg.file_dialog(
            directory_selector=True,
            show=False,
            callback=lambda _s, app_data, _u: self._on_choose_path(self._tag_convert_output, app_data),
            tag=self._tag_convert_dialog_output,
            width=700,
            height=420,
        ):
            dpg.add_file_extension(".*")

        with dpg.window(tag="main_window", label="Small PDF", width=960, height=520):
            with dpg.tab_bar():
                with dpg.tab(label="Cut PDF"):
                    dpg.add_text("PDF source")
                    with dpg.group(horizontal=True):
                        dpg.add_input_text(tag=self._tag_cut_input, width=760)
                        dpg.add_button(label="Choose PDF", callback=lambda: dpg.configure_item(self._tag_cut_dialog_pdf, show=True))

                    dpg.add_spacer(height=8)
                    dpg.add_text("Start page")
                    dpg.add_input_int(tag=self._tag_cut_start, default_value=1, min_value=1, min_clamped=True, width=140)
                    dpg.add_text("End page")
                    dpg.add_input_int(tag=self._tag_cut_end, default_value=1, min_value=1, min_clamped=True, width=140)

                    dpg.add_spacer(height=8)
                    dpg.add_text("Output directory")
                    with dpg.group(horizontal=True):
                        dpg.add_input_text(tag=self._tag_cut_output, width=760)
                        dpg.add_button(label="Choose folder", callback=lambda: dpg.configure_item(self._tag_cut_dialog_output, show=True))

                    dpg.add_spacer(height=12)
                    dpg.add_button(label="Cut PDF", callback=self._on_cut_pdf)

                with dpg.tab(label="PDF -> Markdown"):
                    dpg.add_text("PDF source")
                    with dpg.group(horizontal=True):
                        dpg.add_input_text(tag=self._tag_convert_input, width=760)
                        dpg.add_button(label="Choose PDF", callback=lambda: dpg.configure_item(self._tag_convert_dialog_pdf, show=True))

                    dpg.add_spacer(height=8)
                    dpg.add_text("Markdown output directory")
                    with dpg.group(horizontal=True):
                        dpg.add_input_text(tag=self._tag_convert_output, width=760)
                        dpg.add_button(label="Choose folder", callback=lambda: dpg.configure_item(self._tag_convert_dialog_output, show=True))

                    dpg.add_spacer(height=10)
                    dpg.add_progress_bar(tag=self._tag_convert_progress, default_value=0.0, overlay="")
                    dpg.add_text("", tag=self._tag_convert_status)
                    dpg.add_text("", tag=self._tag_convert_result, wrap=920)

                    dpg.add_spacer(height=10)
                    with dpg.group(horizontal=True):
                        dpg.add_button(
                            label="Convert (Gemini)",
                            tag=self._convert_button_tags[0],
                            callback=self._on_convert_gemini,
                        )

        dpg.set_primary_window("main_window", True)

    def _drain_ui_queue(self) -> None:
        while True:
            try:
                fn = self._ui_queue.get_nowait()
            except queue.Empty:
                break
            fn()

    def _post_ui(self, fn: Callable[[], None]) -> None:
        self._ui_queue.put(fn)

    @staticmethod
    def _path_from_dialog_data(app_data: Any) -> str:
        if not isinstance(app_data, dict):
            return ""
        selections = app_data.get("selections")
        if isinstance(selections, dict) and selections:
            first_key = next(iter(selections))
            value = selections.get(first_key)
            if isinstance(value, str) and value:
                return value
        for key in ("file_path_name", "current_path"):
            value = app_data.get(key)
            if isinstance(value, str) and value:
                return value
        return ""

    def _on_choose_path(self, target_tag: str, app_data: Any) -> None:
        path = self._path_from_dialog_data(app_data)
        if path:
            dpg.set_value(target_tag, path)

    def _show_error(self, message: str) -> None:
        dpg.set_value(self._tag_convert_result, f"[ERROR] {message}")

    def _show_info(self, message: str) -> None:
        dpg.set_value(self._tag_convert_result, message)

    def _set_convert_busy(self, busy: bool) -> None:
        for tag in self._convert_button_tags:
            dpg.configure_item(tag, enabled=not busy)

    def _start_indeterminate(self, status: str) -> None:
        dpg.configure_item(self._tag_convert_progress, overlay="")
        dpg.set_value(self._tag_convert_progress, -1.0)
        dpg.set_value(self._tag_convert_status, status)

    def _set_progress(self, value_0_1: float, status: str) -> None:
        clamped = min(max(value_0_1, 0.0), 1.0)
        dpg.set_value(self._tag_convert_progress, clamped)
        dpg.configure_item(self._tag_convert_progress, overlay=f"{int(clamped * 100)}%")
        dpg.set_value(self._tag_convert_status, status)

    def _reset_progress(self) -> None:
        dpg.set_value(self._tag_convert_progress, 0.0)
        dpg.configure_item(self._tag_convert_progress, overlay="")
        dpg.set_value(self._tag_convert_status, "")

    def _on_cut_pdf(self) -> None:
        input_pdf = str(dpg.get_value(self._tag_cut_input)).strip()
        output_dir = str(dpg.get_value(self._tag_cut_output)).strip()
        start_page = int(dpg.get_value(self._tag_cut_start))
        end_page = int(dpg.get_value(self._tag_cut_end))

        if not input_pdf:
            self._show_error("Please choose a source PDF.")
            return
        if not output_dir:
            self._show_error("Please choose an output directory.")
            return
        if start_page <= 0 or end_page <= 0:
            self._show_error("Start/End page must be positive integers.")
            return

        try:
            output_path = extract_pdf_page_range(input_pdf, start_page, end_page, output_dir)
        except Exception as error:
            self._show_error(f"Cannot cut PDF: {error}")
            return

        self._show_info(f"Cut success. New PDF: {output_path}")

    def _validate_convert_inputs(self) -> tuple[str, str] | None:
        input_pdf = str(dpg.get_value(self._tag_convert_input)).strip()
        output_dir = str(dpg.get_value(self._tag_convert_output)).strip()
        if not input_pdf:
            self._show_error("Please choose a source PDF.")
            return None
        if not output_dir:
            self._show_error("Please choose an output directory.")
            return None
        return input_pdf, output_dir

    def _run_background(self, target: Callable[[], str], mode_label: str) -> None:
        self._set_convert_busy(True)
        dpg.set_value(self._tag_convert_result, "")

        def _run() -> None:
            try:
                output_path = target()
            except Exception as error:
                captured_error = error

                def _finish_error() -> None:
                    self._finish_convert_error(mode_label, captured_error)

                self._post_ui(_finish_error)
                return

            def _finish_success() -> None:
                self._finish_convert_ok(mode_label, output_path)

            self._post_ui(_finish_success)

        threading.Thread(target=_run, daemon=True).start()

    def _finish_convert_ok(self, mode_label: str, output_path: str) -> None:
        self._set_convert_busy(False)
        self._set_progress(1.0, f"Done ({mode_label}).")
        self._show_info(f"Markdown created: {output_path}")

    def _finish_convert_error(self, mode_label: str, error: Exception) -> None:
        self._set_convert_busy(False)
        self._reset_progress()
        self._show_error(f"{mode_label} failed: {error}")

    def _on_convert_gemini(self) -> None:
        validated = self._validate_convert_inputs()
        if validated is None:
            return
        input_pdf, output_dir = validated
        self._set_progress(0.0, "Starting Gemini conversion...")

        def _gemini_progress(processed: int, total: int) -> None:
            total_pages = max(total, 1)
            progress = min(max(processed / total_pages, 0.0), 1.0)
            self._post_ui(
                lambda: self._set_progress(
                    progress,
                    f"Gemini page {processed}/{total_pages}",
                )
            )

        self._run_background(
            lambda: convert_pdf_to_markdown_gemini(
                input_pdf,
                output_dir,
                progress_callback=_gemini_progress,
            ),
            "Gemini",
        )


def main() -> None:
    load_dotenv(Path(__file__).resolve().parents[1] / ".env")
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    SmallPdfApp().run()


if __name__ == "__main__":
    main()
