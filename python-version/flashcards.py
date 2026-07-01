import csv
import random
import sys
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)


class FlashcardApp(QWidget):
    def __init__(self) -> None:
        super().__init__()

        self.selected_files: list[str] = []
        self.current_theme = "dark"

        self.all_cards: list[dict] = []
        self.active_cards: list[dict] = []
        self.wrong_cards: list[dict] = []
        self.current_card: dict | None = None
        self.current_index = 0
        self.round_number = 1
        self.total_correct = 0
        self.total_wrong = 0
        self.original_card_count = 0

        self.setWindowTitle("Flashcard App")
        self.resize(760, 520)
        self.setAcceptDrops(True)

        self.build_ui()
        self.connect_signals()
        self.apply_theme("dark")
        self.reset_session_state()
        self.show_menu_screen()

    def build_ui(self) -> None:
        self.main_layout = QVBoxLayout()
        self.main_layout.setSpacing(16)
        self.main_layout.setContentsMargins(24, 24, 24, 24)
        self.setLayout(self.main_layout)

        self.menu_widget = QWidget()
        menu_layout = QVBoxLayout()
        menu_layout.setSpacing(18)

        self.title_label = QLabel("Flashcard App")
        self.title_label.setObjectName("title")
        self.title_label.setAlignment(Qt.AlignCenter)

        self.info_label = QLabel("A simple CSV-based study utility.")
        self.info_label.setAlignment(Qt.AlignCenter)

        self.files_label = QLabel("Selected files: 0")
        self.files_label.setAlignment(Qt.AlignCenter)
        self.files_label.setWordWrap(True)

        theme_row = QHBoxLayout()
        self.light_button = QPushButton("Light Mode")
        self.dark_button = QPushButton("Dark Mode")
        theme_row.addWidget(self.light_button)
        theme_row.addWidget(self.dark_button)

        action_row = QHBoxLayout()
        self.select_files_button = QPushButton("Select Files")
        self.clear_files_button = QPushButton("Clear Files")
        self.how_to_use_button = QPushButton("How to Use")
        self.start_button = QPushButton("Start")

        action_row.addWidget(self.select_files_button)
        action_row.addWidget(self.clear_files_button)
        action_row.addWidget(self.how_to_use_button)
        action_row.addWidget(self.start_button)

        menu_layout.addStretch()
        menu_layout.addWidget(self.title_label)
        menu_layout.addWidget(self.info_label)
        menu_layout.addLayout(theme_row)
        menu_layout.addLayout(action_row)
        menu_layout.addWidget(self.files_label)
        menu_layout.addStretch()
        self.menu_widget.setLayout(menu_layout)

        self.quiz_widget = QWidget()
        quiz_layout = QVBoxLayout()
        quiz_layout.setSpacing(14)

        self.progress_label = QLabel("")
        self.progress_label.setAlignment(Qt.AlignCenter)

        self.question_label = QLabel("")
        self.question_label.setAlignment(Qt.AlignCenter)
        self.question_label.setWordWrap(True)
        self.question_label.setMinimumHeight(120)

        self.feedback_label = QLabel("")
        self.feedback_label.setAlignment(Qt.AlignCenter)
        self.feedback_label.setWordWrap(True)

        self.show_answer_button = QPushButton("Show Answer")

        self.answer_label = QLabel("")
        self.answer_label.setAlignment(Qt.AlignCenter)
        self.answer_label.setWordWrap(True)

        self.self_mark_row = QHBoxLayout()
        self.right_button = QPushButton("Right")
        self.wrong_button = QPushButton("Wrong")
        self.self_mark_row.addWidget(self.right_button)
        self.self_mark_row.addWidget(self.wrong_button)

        self.choice_buttons: list[QPushButton] = []
        for _ in range(4):
            button = QPushButton("")
            button.setVisible(False)
            self.choice_buttons.append(button)
            quiz_layout.addWidget(button)

        self.next_button = QPushButton("Next")
        self.back_to_menu_button = QPushButton("Back to Menu")

        quiz_layout.addWidget(self.progress_label)
        quiz_layout.addWidget(self.question_label)
        quiz_layout.addWidget(self.feedback_label)
        quiz_layout.addWidget(self.show_answer_button)
        quiz_layout.addWidget(self.answer_label)
        quiz_layout.addLayout(self.self_mark_row)
        quiz_layout.addWidget(self.next_button)
        quiz_layout.addWidget(self.back_to_menu_button)

        self.quiz_widget.setLayout(quiz_layout)

        self.main_layout.addWidget(self.menu_widget)
        self.main_layout.addWidget(self.quiz_widget)

    def connect_signals(self) -> None:
        self.light_button.clicked.connect(lambda: self.apply_theme("light"))
        self.dark_button.clicked.connect(lambda: self.apply_theme("dark"))
        self.select_files_button.clicked.connect(self.select_files)
        self.clear_files_button.clicked.connect(self.clear_files)
        self.how_to_use_button.clicked.connect(self.show_how_to_use)
        self.start_button.clicked.connect(self.start_session)

        self.show_answer_button.clicked.connect(self.show_answer)
        self.right_button.clicked.connect(self.mark_right)
        self.wrong_button.clicked.connect(self.mark_wrong)
        self.next_button.clicked.connect(self.next_card)
        self.back_to_menu_button.clicked.connect(self.return_to_menu)

        for button in self.choice_buttons:
            button.clicked.connect(self.handle_choice_click)

    def apply_theme(self, theme_name: str) -> None:
        self.current_theme = theme_name

        if theme_name == "light":
            self.setStyleSheet(
                """
                QWidget {
                    background-color: #f4f4f4;
                    color: #111111;
                    font-size: 16px;
                }
                QLabel#title {
                    font-size: 28px;
                    font-weight: bold;
                }
                QPushButton {
                    background-color: #dddddd;
                    color: #111111;
                    border: 1px solid #999999;
                    padding: 10px;
                    min-height: 18px;
                }
                QPushButton:hover {
                    background-color: #cccccc;
                }
                """
            )
        else:
            self.setStyleSheet(
                """
                QWidget {
                    background-color: #1c1c1c;
                    color: #f0f0f0;
                    font-size: 16px;
                }
                QLabel#title {
                    font-size: 28px;
                    font-weight: bold;
                }
                QPushButton {
                    background-color: #333333;
                    color: #f0f0f0;
                    border: 1px solid #666666;
                    padding: 10px;
                    min-height: 18px;
                }
                QPushButton:hover {
                    background-color: #444444;
                }
                """
            )

    def show_menu_screen(self) -> None:
        self.menu_widget.show()
        self.quiz_widget.hide()

    def show_quiz_screen(self) -> None:
        self.menu_widget.hide()
        self.quiz_widget.show()

    def select_files(self) -> None:
        files, _ = QFileDialog.getOpenFileNames(
            self,
            "Select CSV Files",
            "",
            "CSV Files (*.csv)",
        )

        if files:
            merged = list(dict.fromkeys(self.selected_files + files))
            self.selected_files = merged
            self.update_files_label()

    def clear_files(self) -> None:
        self.selected_files = []
        self.update_files_label()

    def update_files_label(self) -> None:
        count = len(self.selected_files)
        self.files_label.setText(f"Selected files: {count}")

    def reset_session_state(self) -> None:
        self.all_cards = []
        self.active_cards = []
        self.wrong_cards = []
        self.current_card = None
        self.current_index = 0
        self.round_number = 1
        self.total_correct = 0
        self.total_wrong = 0
        self.original_card_count = 0

        self.progress_label.setText("")
        self.question_label.setText("")
        self.feedback_label.setText("")
        self.answer_label.setText("")

        self.answer_label.hide()
        self.show_answer_button.hide()
        self.next_button.hide()
        self.back_to_menu_button.hide()

        for button in self.choice_buttons:
            button.hide()
            button.setEnabled(True)
            button.setText("")

        self.right_button.hide()
        self.wrong_button.hide()

    def return_to_menu(self) -> None:
        self.reset_session_state()
        self.show_menu_screen()

    def show_how_to_use(self) -> None:
        QMessageBox.information(
            self,
            "How to Use",
            (
                "1. Select one or more CSV files.\n"
                "2. Press Start.\n\n"
                "CSV format:\n\n"
                "Definition card:\n"
                "question,answer\n\n"
                "Multiple-choice card:\n"
                "question,answer,wrong1,wrong2,wrong3\n\n"
                "Invalid files are skipped.\n"
                "Cards answered incorrectly are shown again in later rounds."
            ),
        )

    def start_session(self) -> None:
        if not self.selected_files:
            QMessageBox.warning(
                self,
                "No Files Selected",
                "Select at least one CSV file before starting.",
            )
            return

        cards, skipped_messages = self.load_cards_from_files(self.selected_files)

        if not cards:
            message = "No valid cards were loaded."
            if skipped_messages:
                message += "\n\nSkipped files:\n- " + "\n- ".join(skipped_messages)

            QMessageBox.warning(
                self,
                "No Valid Cards",
                message,
            )
            return

        if skipped_messages:
            QMessageBox.information(
                self,
                "Some Files Were Skipped",
                "The session will continue.\n\nSkipped files:\n- " + "\n- ".join(skipped_messages),
            )

        self.all_cards = cards
        self.original_card_count = len(cards)
        self.active_cards = cards[:]
        random.shuffle(self.active_cards)
        self.wrong_cards = []
        self.current_card = None
        self.current_index = 0
        self.round_number = 1
        self.total_correct = 0
        self.total_wrong = 0

        self.show_quiz_screen()
        self.load_current_card()

    def load_cards_from_files(self, file_paths: list[str]) -> tuple[list[dict], list[str]]:
        cards: list[dict] = []
        skipped_messages: list[str] = []

        for file_path in file_paths:
            file_name = Path(file_path).name

            try:
                with open(file_path, "r", encoding="utf-8-sig", newline="") as csv_file:
                    reader = csv.DictReader(csv_file)

                    if reader.fieldnames is None:
                        skipped_messages.append(f"{file_name}: empty or unreadable CSV")
                        continue

                    normalized_fields = [field.strip().lower() for field in reader.fieldnames if field]
                    if "question" not in normalized_fields or "answer" not in normalized_fields:
                        skipped_messages.append(
                            f"{file_name}: missing required columns (question, answer)"
                        )
                        continue

                    file_card_count = 0

                    for row in reader:
                        normalized_row = {}
                        for key, value in row.items():
                            if key is None:
                                continue
                            normalized_key = key.strip().lower()
                            normalized_value = value.strip() if value else ""
                            normalized_row[normalized_key] = normalized_value

                        question = normalized_row.get("question", "")
                        answer = normalized_row.get("answer", "")

                        if not question or not answer:
                            continue

                        wrong_answers = []
                        for key in ("wrong1", "wrong2", "wrong3"):
                            value = normalized_row.get(key, "")
                            if value:
                                wrong_answers.append(value)

                        card = {
                            "question": question,
                            "answer": answer,
                            "wrong_answers": wrong_answers,
                        }
                        cards.append(card)
                        file_card_count += 1

                    if file_card_count == 0:
                        skipped_messages.append(f"{file_name}: no valid rows found")

            except Exception as exc:
                skipped_messages.append(f"{file_name}: {exc}")

        return cards, skipped_messages

    def load_current_card(self) -> None:
        if self.current_index >= len(self.active_cards):
            self.finish_round()
            return

        self.current_card = self.active_cards[self.current_index]

        self.progress_label.setText(
            f"Round {self.round_number}  |  Card {self.current_index + 1} of {len(self.active_cards)}"
        )
        self.question_label.setText(self.current_card["question"])
        self.feedback_label.setText("")
        self.answer_label.setText("")
        self.answer_label.hide()
        self.show_answer_button.hide()
        self.next_button.hide()
        self.back_to_menu_button.hide()

        for button in self.choice_buttons:
            button.hide()
            button.setEnabled(True)
            button.setText("")

        self.right_button.show()
        self.wrong_button.show()

        wrong_answers = self.current_card["wrong_answers"]
        if wrong_answers:
            self.setup_multiple_choice_card()
        else:
            self.setup_reveal_card()

    def setup_reveal_card(self) -> None:
        self.show_answer_button.show()
        self.right_button.hide()
        self.wrong_button.hide()

    def setup_multiple_choice_card(self) -> None:
        options = self.current_card["wrong_answers"][:] + [self.current_card["answer"]]
        random.shuffle(options)

        for index, option in enumerate(options):
            if index < len(self.choice_buttons):
                self.choice_buttons[index].setText(option)
                self.choice_buttons[index].show()

        self.show_answer_button.hide()
        self.answer_label.hide()
        self.right_button.hide()
        self.wrong_button.hide()

    def show_answer(self) -> None:
        if self.current_card is None:
            return

        self.answer_label.setText(self.current_card["answer"])
        self.answer_label.show()
        self.show_answer_button.hide()
        self.right_button.show()
        self.wrong_button.show()

    def mark_right(self) -> None:
        self.total_correct += 1
        self.feedback_label.setText("Marked right.")
        self.right_button.hide()
        self.wrong_button.hide()
        self.next_button.show()

    def mark_wrong(self) -> None:
        if self.current_card is None:
            return

        self.total_wrong += 1
        self.wrong_cards.append(self.current_card)

        if self.current_card["wrong_answers"]:
            self.feedback_label.setText(f"Wrong. Answer: {self.current_card['answer']}")
        else:
            self.feedback_label.setText("Marked wrong.")

        self.right_button.hide()
        self.wrong_button.hide()
        self.next_button.show()

    def handle_choice_click(self) -> None:
        if self.current_card is None:
            return

        button = self.sender()
        if not isinstance(button, QPushButton):
            return

        selected_text = button.text()
        correct_answer = self.current_card["answer"]

        for choice_button in self.choice_buttons:
            choice_button.setEnabled(False)

        if selected_text == correct_answer:
            self.total_correct += 1
            self.feedback_label.setText("Correct.")
        else:
            self.total_wrong += 1
            self.wrong_cards.append(self.current_card)
            self.feedback_label.setText(f"Wrong. Answer: {correct_answer}")

        self.next_button.show()

    def next_card(self) -> None:
        self.current_index += 1
        self.load_current_card()

    def finish_round(self) -> None:
        if self.wrong_cards:
            self.active_cards = self.wrong_cards[:]
            random.shuffle(self.active_cards)
            self.wrong_cards = []
            self.current_index = 0
            self.round_number += 1

            QMessageBox.information(
                self,
                "Next Round",
                f"Starting round {self.round_number} with the cards you missed.",
            )
            self.load_current_card()
            return

        self.show_results()

    def show_results(self) -> None:
        self.progress_label.setText("Session Finished")
        self.question_label.setText(
            f"Original cards: {self.original_card_count}\n"
            f"Correct answers: {self.total_correct}\n"
            f"Wrong answers: {self.total_wrong}\n"
            f"Rounds played: {self.round_number}"
        )
        self.feedback_label.setText("You did great. All missed cards have been cleared.")
        self.answer_label.hide()
        self.show_answer_button.hide()
        self.right_button.hide()
        self.wrong_button.hide()
        self.next_button.hide()

        for button in self.choice_buttons:
            button.hide()

        self.back_to_menu_button.show()

    def dragEnterEvent(self, event) -> None:
        if event.mimeData().hasUrls():
            urls = event.mimeData().urls()
            if any(url.toLocalFile().lower().endswith(".csv") for url in urls):
                event.acceptProposedAction()
                return
        event.ignore()

    def dropEvent(self, event) -> None:
        urls = event.mimeData().urls()
        new_files: list[str] = []

        for url in urls:
            local_path = url.toLocalFile()
            if local_path.lower().endswith(".csv"):
                new_files.append(local_path)

        if new_files:
            merged = list(dict.fromkeys(self.selected_files + new_files))
            self.selected_files = merged
            self.update_files_label()
            event.acceptProposedAction()
        else:
            event.ignore()


def main() -> None:
    app = QApplication(sys.argv)
    window = FlashcardApp()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()