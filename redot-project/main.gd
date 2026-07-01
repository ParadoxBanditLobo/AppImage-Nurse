extends Control

var selected_files: PackedStringArray = []
var cards: Array = []
var current_index := 0
var correct_count := 0
var wrong_count := 0
var current_card: Dictionary = {}
var current_is_multiple_choice := false

@onready var title_label = $VBoxContainer/TitleLabel
@onready var files_label = $VBoxContainer/FilesLabel
@onready var question_label = $VBoxContainer/QuestionLabel
@onready var show_answer_button = $VBoxContainer/AnswerArea/ShowAnswerButton
@onready var answer_label = $VBoxContainer/AnswerArea/AnswerLabel
@onready var mark_buttons = $VBoxContainer/AnswerArea/MarkButtons
@onready var right_button = $VBoxContainer/AnswerArea/MarkButtons/RightButton
@onready var wrong_button = $VBoxContainer/AnswerArea/MarkButtons/WrongButton
@onready var multiple_choice_area = $VBoxContainer/MultipleChoiceArea
@onready var choice1 = $VBoxContainer/MultipleChoiceArea/Choice1
@onready var choice2 = $VBoxContainer/MultipleChoiceArea/Choice2
@onready var choice3 = $VBoxContainer/MultipleChoiceArea/Choice3
@onready var choice4 = $VBoxContainer/MultipleChoiceArea/Choice4
@onready var next_button = $VBoxContainer/NextButton
@onready var result_label = $VBoxContainer/ResultLabel
@onready var file_dialog = $FileDialog
@onready var light_button = $VBoxContainer/ThemeButtons/LightButton
@onready var dark_button = $VBoxContainer/ThemeButtons/DarkButton
@onready var add_files_button = $VBoxContainer/FileButtons/AddFilesButton
@onready var start_button = $VBoxContainer/FileButtons/StartButton

func _ready():
	print("ready")
	hide_quiz_ui()

	add_files_button.pressed.connect(_on_add_files_pressed)
	start_button.pressed.connect(_on_start_pressed)
	file_dialog.files_selected.connect(_on_files_selected)

	light_button.pressed.connect(_on_light_pressed)
	dark_button.pressed.connect(_on_dark_pressed)

	show_answer_button.pressed.connect(_on_show_answer_pressed)
	right_button.pressed.connect(_on_right_pressed)
	wrong_button.pressed.connect(_on_wrong_pressed)
	next_button.pressed.connect(_on_next_pressed)

	choice1.pressed.connect(func(): _on_choice_pressed(choice1.text))
	choice2.pressed.connect(func(): _on_choice_pressed(choice2.text))
	choice3.pressed.connect(func(): _on_choice_pressed(choice3.text))
	choice4.pressed.connect(func(): _on_choice_pressed(choice4.text))

	answer_label.visible = false
	mark_buttons.visible = false
	next_button.visible = false
	result_label.text = ""

func hide_quiz_ui():
	question_label.visible = false
	$VBoxContainer/AnswerArea.visible = false
	multiple_choice_area.visible = false
	next_button.visible = false
	result_label.visible = true

func show_quiz_ui():
	question_label.visible = true

func _on_light_pressed():
	var bg = StyleBoxFlat.new()
	bg.bg_color = Color(0.95, 0.95, 0.95)
	add_theme_stylebox_override("panel", bg)
	modulate = Color(1, 1, 1)

func _on_dark_pressed():
	var bg = StyleBoxFlat.new()
	bg.bg_color = Color(0.12, 0.12, 0.12)
	add_theme_stylebox_override("panel", bg)
	modulate = Color(0.9, 0.9, 0.9)

func _on_add_files_pressed():
	file_dialog.clear_filters()
	file_dialog.add_filter("*.csv ; CSV Files")
	file_dialog.popup_centered_ratio()

func _on_files_selected(paths: PackedStringArray):
	selected_files = paths
	files_label.text = "Selected Files: " + str(selected_files.size())

func _on_start_pressed():
	if selected_files.is_empty():
		result_label.text = "Please select at least one CSV file."
		return

	cards.clear()
	current_index = 0
	correct_count = 0
	wrong_count = 0

	for path in selected_files:
		load_csv_file(path)

	if cards.is_empty():
		result_label.text = "No valid cards found."
		return

	cards.shuffle()
	show_quiz_ui()
	show_card()

func load_csv_file(path: String):
	var file = FileAccess.open(path, FileAccess.READ)
	if file == null:
		return

	if file.eof_reached():
		return

	var header_line = file.get_line()
	var headers = parse_csv_line(header_line)

	while not file.eof_reached():
		var line = file.get_line()
		if line.strip_edges() == "":
			continue

		var values = parse_csv_line(line)
		var row = {}

		for i in range(min(headers.size(), values.size())):
			row[headers[i].strip_edges().to_lower()] = values[i].strip_edges()

		if row.has("question") and row.has("answer"):
			cards.append(row)

	file.close()

func parse_csv_line(line: String) -> Array:
	return line.split(",")

func show_card():
	if current_index >= cards.size():
		show_results()
		return

	current_card = cards[current_index]
	question_label.text = current_card.get("question", "")
	result_label.text = "Card %d of %d" % [current_index + 1, cards.size()]

	answer_label.text = current_card.get("answer", "")
	answer_label.visible = false
	mark_buttons.visible = false
	next_button.visible = false

	var wrong_answers: Array = []
	if current_card.has("wrong1") and current_card["wrong1"] != "":
		wrong_answers.append(current_card["wrong1"])
	if current_card.has("wrong2") and current_card["wrong2"] != "":
		wrong_answers.append(current_card["wrong2"])
	if current_card.has("wrong3") and current_card["wrong3"] != "":
		wrong_answers.append(current_card["wrong3"])

	current_is_multiple_choice = wrong_answers.size() > 0

	if current_is_multiple_choice:
		$VBoxContainer/AnswerArea.visible = false
		multiple_choice_area.visible = true

		var options = wrong_answers.duplicate()
		options.append(current_card["answer"])
		options.shuffle()

		var buttons = [choice1, choice2, choice3, choice4]
		for i in range(buttons.size()):
			if i < options.size():
				buttons[i].visible = true
				buttons[i].text = options[i]
				buttons[i].disabled = false
			else:
				buttons[i].visible = false
	else:
		$VBoxContainer/AnswerArea.visible = true
		multiple_choice_area.visible = false

func _on_show_answer_pressed():
	answer_label.visible = true
	mark_buttons.visible = true

func _on_right_pressed():
	correct_count += 1
	next_button.visible = true
	mark_buttons.visible = false

func _on_wrong_pressed():
	wrong_count += 1
	next_button.visible = true
	mark_buttons.visible = false

func _on_choice_pressed(selected_text: String):
	var is_correct = selected_text == current_card.get("answer", "")

	var buttons = [choice1, choice2, choice3, choice4]
	for button in buttons:
		button.disabled = true

	if is_correct:
		result_label.text = "Correct"
		correct_count += 1
	else:
		result_label.text = "Wrong. Correct answer: " + current_card.get("answer", "")
		wrong_count += 1

	next_button.visible = true

func _on_next_pressed():
	current_index += 1
	show_card()

func show_results():
	question_label.text = "Finished"
	$VBoxContainer/AnswerArea.visible = false
	multiple_choice_area.visible = false
	next_button.visible = false

	var total = correct_count + wrong_count
	var percent := 0.0
	if total > 0:
		percent = float(correct_count) / float(total) * 100.0

	result_label.text = "Total: %d | Correct: %d | Wrong: %d | Score: %.1f%%" % [total, correct_count, wrong_count, percent]
