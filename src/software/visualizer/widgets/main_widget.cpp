#include "software/visualizer/widgets/main_widget.h"

#include "software/visualizer/geom/geometry_conversion.h"

MainWidget::MainWidget(QWidget* parent)
    : QWidget(parent), main_widget(new Ui::AutoGeneratedMainWidget())
{
    // Handles all the setup of the generated UI components and adds the components
    // to this widget
    main_widget->setupUi(this);
    scene    = new QGraphicsScene(main_widget->ai_visualization_graphics_view);
    glWidget = new QOpenGLWidget(this);

    // StrongFocus means that the MainWidget will more aggressively capture focus when
    // clicked. Specifically, we do this so that when the user clicks outside of the
    // QLineEdits used for Parameters, the QLineEdit will lose focus.
    // https://www.qtcentre.org/threads/41128-Need-to-implement-in-place-line-edit-unable-to-get-lose-focus-of-QLineEdit
    setFocusPolicy(Qt::StrongFocus);

    // This is a trick to force the initial width of the ai control tabs to be small,
    // and the initial width of the ai view to be large. This sets the sizes of the
    // widgets in the splitter to be unrealistically small (1 pixel) so that the
    // size policies defined for the widgets will take over and grow the widgets to
    // their minimum size, and then distribute the rest of the space according to the
    // policies.
    // See https://doc.qt.io/archives/qt-4.8/qsplitter.html#setSizes
    int number_of_widgets_in_splitter =
        main_widget->ai_control_and_view_splitter->count();
    auto widget_sizes_vector  = std::vector<int>(number_of_widgets_in_splitter, 1);
    auto widget_sizes_qvector = QVector<int>::fromStdVector(widget_sizes_vector);
    auto widget_sizes_list    = QList<int>::fromVector(widget_sizes_qvector);
    main_widget->ai_control_and_view_splitter->setSizes(widget_sizes_list);
    setupSceneView(main_widget->ai_visualization_graphics_view, scene, glWidget);

    setupRobotStatusTable(main_widget->robot_status_table_widget);
    setupAIControls(main_widget);
    setupParametersTab(main_widget);
    // Update to make sure all layout changes apply nicely
    update();
}

void MainWidget::draw(WorldDrawFunction world_draw_function,
                      AIDrawFunction ai_draw_function)
{
    scene->clear();
    world_draw_function.execute(scene);
    ai_draw_function.execute(scene);
}

void MainWidget::setDrawViewArea(const QRectF& new_view_area)
{
    scene->setSceneRect(new_view_area);
    main_widget->ai_visualization_graphics_view->fitInView(scene->sceneRect(),
                                                           Qt::KeepAspectRatio);
}

void MainWidget::updatePlayInfo(const PlayInfo& play_info)
{
    QString play_type_string =
        QString("Play Type: %1\n").arg(QString::fromStdString(play_info.play_type));
    QString play_name_string =
        QString("Play Name: %1\n").arg(QString::fromStdString(play_info.play_name));
    QString tactics_string = QString("Tactics:\n");
    for (const auto& tactic_string : play_info.robot_tactic_assignment)
    {
        tactics_string.append(QString::fromStdString(tactic_string)).append("\n");
    }

    QString play_info_string =
        QString("%1\n%2\n%3").arg(play_type_string, play_name_string, tactics_string);

    main_widget->play_and_tactic_info_text_edit->setText(play_info_string);
}

void MainWidget::updateRobotStatus(const RobotStatus& robot_status)
{
    main_widget->robot_status_table_widget->updateRobotStatus(robot_status);
}
